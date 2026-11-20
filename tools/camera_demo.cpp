// Prepare and start a single camera service without a shell or Python runtime.
#include "camera_process.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <sys/stat.h>

namespace fs=std::filesystem;
using namespace camera;
static std::string env(const char *key,const std::string& fallback="") {
    const char *s=getenv(key); return s && *s ? s : fallback;
}
static std::string trim(std::string s) {
    auto a=s.find_first_not_of(" \t\r\n"), b=s.find_last_not_of(" \t\r\n");
    return a==std::string::npos ? "" : s.substr(a,b-a+1);
}
static void load(const fs::path& path,std::map<std::string,std::string>& cfg) {
    std::ifstream f(path); std::string line;
    if(!f) throw std::runtime_error("cannot read config: "+path.string());
    while(std::getline(f,line)) {
        line=trim(line); if(line.empty() || line[0]=='#') continue;
        auto pos=line.find('=');
        if(pos==std::string::npos) throw std::runtime_error("invalid config line");
        auto key=trim(line.substr(0,pos)), value=trim(line.substr(pos+1));
        if(key.empty() || key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string::npos ||
           value.empty() || value.find_first_of(" \t\"'\\$;&|<>")!=std::string::npos ||
           value.find(char(96))!=std::string::npos)
            throw std::runtime_error("invalid config assignment: "+key);
        cfg[key]=value;
    }
    if(f.bad()) throw std::runtime_error("config read failed");
}
static void write_file(const fs::path& path,const std::string& content) {
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(fd<0) throw std::runtime_error("cannot create "+path.string());
    size_t at=0;
    while(at<content.size()) {
        ssize_t n=write(fd,content.data()+at,content.size()-at);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { close(fd); throw std::runtime_error("cannot write "+path.string()); }
        at+=static_cast<size_t>(n);
    }
    if(close(fd)) throw std::runtime_error("cannot close "+path.string());
}
static int number(const std::string& s,int low,int high,const std::string& field) {
    if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("invalid "+field);
    size_t p=0; long n=std::stol(s,&p);
    if(p!=s.size() || n<low || n>high) throw std::runtime_error("invalid "+field);
    return static_cast<int>(n);
}
static bool executable_on_path(const std::string& name) {
    std::istringstream paths(env("PATH","/usr/bin:/bin"));
    std::string part;
    while(std::getline(paths,part,':')) {
        if(part.empty()) part=".";
        if(access((fs::path(part)/name).c_str(),X_OK)==0) return true;
    }
    return false;
}
int main(int argc,char** argv) {
    if(argc!=4 || (std::string(argv[1])!="imx415-isp" && std::string(argv[1])!="ov8858-isp") ||
       (std::string(argv[2])!="manual" && std::string(argv[2])!="detect" && std::string(argv[2])!="entrance") || !fs::path(argv[3]).is_absolute()) {
        std::cerr << "usage: camera_demo PROFILE entrance|manual|detect ABSOLUTE_NEW_DIRECTORY\n"; return 2;
    }
    try {
        std::string profile=argv[1], mode=argv[2]; fs::path dir=argv[3];
        const bool entrance = mode == "entrance";
        if (entrance) {
            if (profile != "imx415-isp") throw std::runtime_error("entrance requires imx415-isp");
            mode = "detect";
        }
        fs::path root=env("CAMERA_ROOT",fs::canonical("/proc/self/exe").parent_path().string());
        std::map<std::string,std::string> cfg;
        load(root/"config/final-demo.env",cfg); load(root/("config/"+profile+".env"),cfg);
        for(const auto *key:{"MODEL","MODEL_SHA256","WIDTH","HEIGHT","FPS","GOP","PRE_SEC","POST_SEC",
                            "RING_MB","KBPS","MAX_EVENT_SEC","COOLDOWN_SEC","DISK_MIN_MB","DET_SCORE",
                            "DET_FRAMES","RTSP_PORT","RTSP_CLIENTS","RTSP_DEADLINE"})
            if(!cfg.count(key)) throw std::runtime_error(std::string("missing config: ")+key);
        std::string port=env("RTSP_PORT_OVERRIDE",cfg["RTSP_PORT"]);
        number(port,1,65535,"RTSP port");
        std::string encoder=env("ENCODER_INPUT","dmabuf");
        std::string npu=env("NPU_INPUT",mode=="detect"?"dmabuf":"copy");
        if(encoder!="copy" && encoder!="dmabuf") throw std::runtime_error("invalid ENCODER_INPUT");
        if(npu!="copy" && npu!="dmabuf") throw std::runtime_error("invalid NPU_INPUT");
        if(mode!="detect" && npu=="dmabuf") throw std::runtime_error("NPU_INPUT=dmabuf requires detect mode");
        std::string interval=env("DETECT_EVERY","1"); number(interval,1,120,"DETECT_EVERY");
        std::string pipeline=env("DETECT_PIPELINE","0"); number(pipeline,0,1,"DETECT_PIPELINE");
        const char *side_default = entrance ? "1" : "0";
        std::string selfpath=env("DETECT_SELFPATH",side_default); number(selfpath,0,1,"DETECT_SELFPATH");
        std::string contiguous=env("SELFPATH_CONTIGUOUS",side_default); number(contiguous,0,1,"SELFPATH_CONTIGUOUS");
        std::string result_poll=env("SELFPATH_POLL",side_default); number(result_poll,0,1,"SELFPATH_POLL");
        if (entrance && (encoder!="dmabuf" || selfpath!="1" || contiguous!="1" || result_poll!="1"))
            throw std::runtime_error("entrance requires shared encoder input, selfpath, contiguous scan and joint poll");
        if(result_poll=="1" && selfpath!="1") throw std::runtime_error("SELFPATH_POLL requires DETECT_SELFPATH");
        if(contiguous=="1" && selfpath!="1") throw std::runtime_error("SELFPATH_CONTIGUOUS requires DETECT_SELFPATH");
        if(selfpath=="1" && (profile!="imx415-isp" || mode!="detect" || pipeline!="0" || npu!="dmabuf" || interval!="1"))
            throw std::runtime_error("DETECT_SELFPATH requires imx415-isp detect, dmabuf, every=1 and DETECT_PIPELINE=0");
        if(pipeline=="1" && (mode!="detect" || npu!="dmabuf" || interval!="1"))
            throw std::runtime_error("DETECT_PIPELINE=1 requires detect, NPU_INPUT=dmabuf and DETECT_EVERY=1");
        std::string metrics=env("FRAME_METRICS");
        if(!metrics.empty() && !fs::path(metrics).is_absolute())
            throw std::runtime_error("FRAME_METRICS must be an absolute path");
        fs::path binary=env("EVREC_BIN",(root/"evrec").string());
        if(access(binary.c_str(),X_OK)) throw std::runtime_error("missing executable: "+binary.string());
        for(const auto *program:{"camera_service","camera_delivery","resolve_camera"}) {
            auto path=root/program;
            if(access(path.c_str(),X_OK)) throw std::runtime_error("missing executable: "+path.string());
        }
        auto help=run({binary.string(),"--help"},10);
        std::string helptext=help.out+help.err;
        if(!metrics.empty() && helptext.find("--frame-metrics")==std::string::npos)
            throw std::runtime_error("evrec does not support frame observations");
        if(encoder=="dmabuf" && helptext.find("--encoder-input")==std::string::npos)
            throw std::runtime_error("evrec does not support dmabuf encoding");
        if(npu=="dmabuf" && helptext.find("--npu-input")==std::string::npos)
            throw std::runtime_error("evrec does not support dmabuf NPU input");
        if(mode=="detect" && interval!="1" && helptext.find("--detect-every")==std::string::npos)
            throw std::runtime_error("evrec does not support detection sampling");
        if(pipeline=="1" && helptext.find("--detect-pipeline")==std::string::npos)
            throw std::runtime_error("evrec does not support detection pipeline");
        auto video=trim(checked({(root/"resolve_camera").string(),profile},15));
        std::string side;
        if(selfpath=="1") {
            if(result_poll=="1" && helptext.find("--selfpath-poll")==std::string::npos)
                throw std::runtime_error("missing selfpath result polling support");
            if(contiguous=="1" && helptext.find("--selfpath-contiguous")==std::string::npos)
                throw std::runtime_error("missing contiguous scan support");
            if(helptext.find("--detect-selfpath")==std::string::npos) throw std::runtime_error("missing selfpath support");
            side=trim(checked({(root/"resolve_camera").string(),profile,"--selfpath"},15));
            if(side.empty() || side==video) throw std::runtime_error("invalid selfpath");
        }
        struct stat st{}; if(stat(video.c_str(),&st) || !S_ISCHR(st.st_mode)) throw std::runtime_error("invalid capture device: "+video);
        auto aiq=run({"pgrep","-f","^/usr/bin/rkaiq_3A_server([[:space:]]|$)"},10);
        std::istringstream aiq_lines(aiq.out); std::string line; int count=0;
        while(std::getline(aiq_lines,line)) if(!trim(line).empty()) ++count;
        if(aiq.status || count!=1) throw std::runtime_error("expected exactly one running rkaiq_3A_server");
        auto busy=run({"fuser",video},10);
        if(busy.status==0) throw std::runtime_error("capture device already in use: "+video);
        if(busy.status!=1 || !busy.err.empty()) throw std::runtime_error("cannot verify capture device usage: "+busy.err);
        auto sockets=checked({"ss","-H","-ltn","sport = :"+port},10);
        if(!trim(sockets).empty()) throw std::runtime_error("RTSP port already in use");
        if(mode=="detect") {
            auto hash=checked({"sha256sum","--",cfg["MODEL"]},30);
            if(hash.substr(0,64)!=cfg["MODEL_SHA256"]) throw std::runtime_error("model hash mismatch");
        }
        for(const auto *tool:{"ffmpeg","ffprobe","sha256sum"})
            if(!executable_on_path(tool)) throw std::runtime_error(std::string("missing tool: ")+tool);
        if(mkdir(dir.c_str(),0700)) throw std::runtime_error("cannot create new run directory");
        write_file(dir/"kernel.txt",checked({"uname","-a"},10));
        write_file(dir/"board-clock.txt",checked({"date","-u"},10));
        std::string sums=checked({"sha256sum","--",binary.string(),(root/"config/final-demo.env").string(),
                                  (root/("config/"+profile+".env")).string()},30);
        write_file(dir/"inputs.sha256",sums); write_file(dir/"profile.txt",profile+"\n");
        std::vector<std::string> args={binary.string(),"-d",video,"-w",cfg["WIDTH"],"-h",cfg["HEIGHT"],
            "--fps",cfg["FPS"],"--gop",cfg["GOP"],"--pre",cfg["PRE_SEC"],"--post",cfg["POST_SEC"],
            "--ring-mb",cfg["RING_MB"],"--kbps",cfg["KBPS"],"--max-event-sec",cfg["MAX_EVENT_SEC"],
            "--cooldown",cfg["COOLDOWN_SEC"],"--disk-min-mb",cfg["DISK_MIN_MB"],"--recover","1",
            "--max-events",env("MAX_EVENTS","0"),"--rtsp",port,"--rtsp-clients",cfg["RTSP_CLIENTS"],
            "--rtsp-deadline-s",cfg["RTSP_DEADLINE"],"--osd","-o",(dir/"event").string()};
        if(encoder=="dmabuf") { args.push_back("--encoder-input"); args.push_back("dmabuf"); }
        if(!metrics.empty()) args.insert(args.end(),{"--frame-metrics",metrics});
        if(mode=="detect") {
            args.insert(args.end(),{"--detect","--model",cfg["MODEL"],"--det-score",cfg["DET_SCORE"],"--det-frames",cfg["DET_FRAMES"]});
            if(interval!="1") args.insert(args.end(),{"--detect-every",interval});
            if(pipeline=="1") args.push_back("--detect-pipeline");
            if(!side.empty()) args.insert(args.end(),{"--detect-selfpath",side});
            if(contiguous=="1") args.push_back("--selfpath-contiguous");
            if(result_poll=="1") args.push_back("--selfpath-poll");
            if(npu=="dmabuf") args.insert(args.end(),{"--npu-input","dmabuf"});
        } else args.insert(args.end(),{"--trigger",(dir/"trigger").string()});
        std::string recorded; for(const auto& a:args) recorded+=a+"\n"; write_file(dir/"argv.txt",recorded);
        std::vector<std::string> service={(root/"camera_service").string(),dir.string(),"--"};
        service.insert(service.end(),args.begin(),args.end());
        std::vector<char*> av; for(auto& a:service) av.push_back(a.data()); av.push_back(nullptr);
        execv(av[0],av.data()); throw std::runtime_error("cannot start camera_service");
    } catch(const std::exception& e) { std::cerr << "[camera_demo] " << e.what() << '\n'; return 1; }
}
