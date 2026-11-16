// Supervise evrec and deliver finished events without blocking capture.
#include "camera_json.h"
#include "camera_process.h"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs=std::filesystem;
using namespace camera;
static volatile sig_atomic_t stop_signal=0;
static void on_signal(int s) { stop_signal=s; }
static std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if(!f) throw std::runtime_error("cannot read metadata");
    std::string s((std::istreambuf_iterator<char>(f)), {});
    if(f.bad() || s.size()>4*1024*1024) throw std::runtime_error("invalid metadata");
    return s;
}
static bool ends(const std::string& s,const std::string& suffix) {
    return s.size()>=suffix.size() && s.compare(s.size()-suffix.size(),suffix.size(),suffix)==0;
}
static bool terminate_group(pid_t pid) {
    if(pid<=0) return true;
    kill(-pid,SIGKILL); kill(pid,SIGKILL);
    auto deadline=Clock::now()+std::chrono::seconds(2);
    while(Clock::now()<deadline) {
        int st=0; auto n=waitpid(pid,&st,WNOHANG);
        if(n==pid || (n<0 && errno==ECHILD)) return true;
        if(n<0 && errno!=EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
int main(int argc,char** argv) {
    if(argc<4 || std::string(argv[2])!="--" || !fs::is_directory(argv[1])) {
        std::cerr << "usage: camera_service RUN_DIRECTORY -- EVREC [ARGS...]\n"; return 2;
    }
    fs::path dir=fs::absolute(argv[1]);
    fs::path exe=fs::canonical("/proc/self/exe");
    fs::path delivery=exe.parent_path()/"camera_delivery";
    struct sigaction sa{}; sa.sa_handler=on_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,nullptr); sigaction(SIGTERM,&sa,nullptr);
    std::vector<std::string> command;
    for(int i=3;i<argc;++i) command.emplace_back(argv[i]);
    pid_t recorder=-1, worker=-1; int recorder_status=0; bool reaped=false, failed=false, stopping=false;
    auto stop_at=Clock::now(), worker_at=Clock::now();
    std::deque<fs::path> pending; std::set<fs::path> seen;
    try {
        recorder=spawn(command);
        std::ofstream pidfile(dir/"producer.pid");
        pidfile << recorder << '\n'; pidfile.close();
        if(!pidfile) throw std::runtime_error("cannot publish producer.pid");
        auto scan=[&](bool final) {
            std::vector<fs::path> entries;
            for(const auto& e:fs::directory_iterator(dir)) {
                auto n=e.path().filename().string();
                if(n.rfind("event",0)==0 && ends(n,".json") &&
                   !ends(n,".delivery.json") && !ends(n,".pending.json") && !seen.count(e.path()))
                    entries.push_back(e.path());
            }
            std::sort(entries.begin(),entries.end());
            for(const auto& p:entries) {
                try { (void)parse_json(read_all(p)); }
                catch(const std::exception&) { if(!final) continue; }
                if(pending.size()+(worker>0?1:0)>=32) throw std::runtime_error("delivery queue capacity exceeded");
                pending.push_back(p); seen.insert(p);
            }
        };
        auto step=[&]() {
            if(worker>0) {
                int st=0; auto n=waitpid(worker,&st,WNOHANG);
                if(n==worker) { if(status_code(st)) failed=true; worker=-1; }
                else if(n<0 && errno!=EINTR) throw std::runtime_error("delivery wait failed");
                else if(Clock::now()-worker_at>std::chrono::seconds(120)) {
                    std::cerr << "[delivery] worker timed out\n";
                    if(!terminate_group(worker)) throw std::runtime_error("delivery worker did not exit after kill");
                    worker=-1; failed=true;
                }
            }
            if(worker<0 && !pending.empty()) {
                auto item=pending.front(); pending.pop_front();
                worker=spawn({delivery.string(),item.string()},-1,-1,true); worker_at=Clock::now();
            }
        };
        while(!reaped) {
            int st=0; auto n=waitpid(recorder,&st,WNOHANG);
            if(n==recorder) { recorder_status=st; reaped=true; break; }
            if(n<0 && errno!=EINTR) throw std::runtime_error("recorder wait failed");
            if(stop_signal && !stopping) { kill(recorder,SIGTERM); stopping=true; stop_at=Clock::now(); }
            if(stopping && Clock::now()-stop_at>std::chrono::seconds(20))
                throw std::runtime_error("recorder did not stop; inspect device state");
            scan(false); step(); std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        scan(true); auto deadline=Clock::now()+std::chrono::seconds(120);
        while(worker>0 || !pending.empty()) {
            if(Clock::now()>deadline) throw std::runtime_error("delivery shutdown deadline exceeded");
            step(); std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        for(const auto& e:fs::directory_iterator(dir)) {
            auto n=e.path().filename().string();
            if(n.rfind("event",0)!=0) continue;
            if(ends(n,".part") || ends(n,".pending.json")) failed=true;
            if(ends(n,".h264")) { auto metadata=e.path(); metadata.replace_extension(".json"); if(!fs::exists(metadata)) failed=true; }
        }
        return failed ? 1 : status_code(recorder_status);
    } catch(const std::exception& e) {
        std::cerr << "[service] " << e.what() << '\n';
        if(worker>0 && !terminate_group(worker))
            std::cerr << "[service] delivery worker PID " << worker << " is still alive\n";
        if(recorder>0 && !reaped) {
            kill(recorder,SIGTERM); auto deadline=Clock::now()+std::chrono::seconds(20);
            while(Clock::now()<deadline) {
                int st=0; auto n=waitpid(recorder,&st,WNOHANG);
                if(n==recorder || (n<0 && errno!=EINTR)) { reaped=true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if(!reaped) std::cerr << "[service] recorder PID " << recorder << " is still alive\n";
        }
        return 1;
    }
}
