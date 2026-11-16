// Validate a completed evrec segment and publish its MP4 delivery status.
#include "camera_json.h"
#include "camera_process.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace camera;
static std::string read_file(const fs::path& p, size_t max = 4*1024*1024) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + p.string());
    std::string s((std::istreambuf_iterator<char>(f)), {});
    if (f.bad() || s.size() > max) throw std::runtime_error("invalid or oversized input: " + p.string());
    return s;
}
static std::string sha(const fs::path& p) {
    std::istringstream in(checked({"sha256sum", "--", p.string()}));
    std::string hash; in >> hash;
    if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("invalid sha256sum output");
    return hash;
}
static long frame_count(const std::string& probe, const fs::path& p) {
    auto r = run({probe,"-v","error","-count_frames","-select_streams","v:0",
                  "-show_entries","stream=codec_name,nb_read_frames","-of","compact=p=0:nk=0",p.string()});
    if (r.status || !r.err.empty()) throw std::runtime_error("ffprobe failed: " + r.err);
    std::istringstream lines(r.out); std::string line, row; int rows=0;
    while (std::getline(lines,line)) {
        if (line.rfind("codec_name=",0)==0) { row=line; ++rows; }
        else if (!line.empty()) std::cerr << "[ffprobe] " << line << '\n';
    }
    if (rows != 1 || row.rfind("codec_name=h264|nb_read_frames=",0)!=0)
        throw std::runtime_error("expected one H.264 stream with a decoded frame count");
    auto n = row.substr(std::string("codec_name=h264|nb_read_frames=").size());
    size_t pos=0; long count = std::stol(n,&pos);
    if (pos != n.size() || count <= 0) throw std::runtime_error("invalid frame count");
    return count;
}
static std::vector<std::string> split(const std::string& s) {
    std::istringstream in(s); std::vector<std::string> fields; std::string part;
    while (std::getline(in,part,',')) fields.push_back(part);
    return fields;
}
static long long integer(const std::string& s) {
    size_t n=0; long long v=std::stoll(s,&n); if(n!=s.size()) throw std::runtime_error("invalid integer"); return v;
}
static double decimal(const std::string& s) {
    size_t n=0; double v=std::stod(s,&n);
    if(n!=s.size() || !std::isfinite(v)) throw std::runtime_error("invalid timestamp");
    return v;
}
static long packet_frames(const fs::path& p, long long size) {
    std::ifstream f(p); std::string line;
    if (!std::getline(f,line) || line != "frame_seq,packet_index,media_ms,capture_ms,bytes,decodable_start")
        throw std::runtime_error("invalid packet log header");
    long frames=0; long long bytes=0, previous_seq=-1; double previous=-1; bool first=true;
    while (std::getline(f,line)) {
        auto a=split(line); if(a.size()!=6) throw std::runtime_error("invalid packet log");
        auto seq=integer(a[0]), len=integer(a[4]); auto media=decimal(a[2]), capture=decimal(a[3]);
        if (seq < 0 || len <= 0 || bytes > size-len || std::abs(media-capture)>0.001 ||
            (!first && capture < previous) || (first && a[5]!="1"))
            throw std::runtime_error("invalid packet timing, size, or start");
        if (first || seq != previous_seq) ++frames;
        first=false; previous=capture; previous_seq=seq; bytes+=len;
    }
    if (f.bad() || first || bytes!=size) throw std::runtime_error("packet record does not match video byte count");
    return frames;
}
static void publish(const fs::path& path, const std::string& text) {
    auto pending=path.string()+".part";
    int fd=open(pending.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(fd<0) throw std::runtime_error("cannot create delivery result");
    size_t pos=0;
    while(pos<text.size()) {
        ssize_t n=write(fd,text.data()+pos,text.size()-pos);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { close(fd); throw std::runtime_error("cannot write delivery result"); }
        pos+=static_cast<size_t>(n);
    }
    if(close(fd)) throw std::runtime_error("cannot close delivery result");
    if(link(pending.c_str(),path.c_str())) throw std::runtime_error("cannot publish delivery result");
    if(unlink(pending.c_str())) throw std::runtime_error("cannot remove pending delivery result");
}
static int finalize(const fs::path& metadata) {
    fs::path source=metadata; source.replace_extension(".h264");
    fs::path output=metadata; output.replace_extension(".mp4");
    fs::path pending=metadata; pending.replace_extension(".pending.mp4");
    fs::path report=metadata; report.replace_extension(".delivery.json");
    std::string extra, error; bool success=false;
    try {
        auto data=parse_json(read_file(metadata));
        if(data.at("write_error").type!=Json::Bool || data.at("write_error").boolean)
            throw std::runtime_error("recorder did not report a clean close");
        const auto &size_field=data.at("bytes");
        if(size_field.type!=Json::Number || size_field.text.find_first_of(".eE")!=std::string::npos)
            throw std::runtime_error("invalid recorder byte count");
        auto size=integer(size_field.text);
        if(size<=0 || fs::file_size(source)!=static_cast<uintmax_t>(size))
            throw std::runtime_error("recorded video size differs from metadata");
        long packets=-1;
        if(data.object.count("schema") && data.at("schema").number()==2) {
            const auto &ts=data.at("timestamp_source");
            if(ts.type!=Json::String || (ts.text!="v4l2-monotonic" && ts.text!="dqbuf-monotonic"))
                throw std::runtime_error("invalid timestamp source");
            auto csv=metadata; csv.replace_extension(".packets.csv");
            packets=packet_frames(csv,size);
            extra += ",\n  \"packet_log_sha256\": " + json_quote(sha(csv));
        }
        if(fs::exists(output)||fs::exists(pending)) throw std::runtime_error("MP4 output or unfinished conversion already exists");
        double first=data.at("first_ts_ms").number(), last=data.at("last_ts_ms").number();
        if(last<=first) throw std::runtime_error("invalid packet time span");
        long count=frame_count("ffprobe",source);
        if(count<2 || (packets>=0 && packets!=count)) throw std::runtime_error("decoded frame count mismatch");
        // Recorder timestamps have millisecond values with three decimal places.
        long long den=std::llround((last-first)*1000.0), num=(count-1)*1000000LL;
        if(den<=0 || num<=0 || static_cast<double>(num)/den>240) throw std::runtime_error("invalid estimated frame rate");
        auto div=std::gcd(num,den); num/=div; den/=div;
        std::string rate=std::to_string(num) + (den==1 ? "" : "/" + std::to_string(den));
        auto run_result=run({"ffmpeg","-nostdin","-hide_banner","-n","-r",rate,"-f","h264","-i",source.string(),
                             "-map","0:v:0","-c:v","copy","-movflags","+faststart",pending.string()});
        if(run_result.status) throw std::runtime_error("ffmpeg failed: "+run_result.err);
        if(frame_count("ffprobe",pending)!=count) throw std::runtime_error("MP4 decoded frame count mismatch");
        if(link(pending.c_str(),output.c_str()) || unlink(pending.c_str()))
            throw std::runtime_error("cannot publish MP4");
        extra += ",\n  \"video_frames\": "+std::to_string(count)+",\n  \"playback_fps\": "+json_quote(rate)+
                 ",\n  \"timing\": \"average-rate approximation; individual capture timestamps not preserved\""+
                 ",\n  \"source_sha256\": "+json_quote(sha(source))+
                 ",\n  \"metadata_sha256\": "+json_quote(sha(metadata))+
                 ",\n  \"output_sha256\": "+json_quote(sha(output));
        for(const auto& k: {"event_id","segment_index"}) {
            auto it=data.object.find(k);
            if(it!=data.object.end() && it->second.type==Json::Number) extra+=",\n  "+json_quote(k)+": "+it->second.text;
        }
        if(data.object.count("close_reason") && data.at("close_reason").type==Json::String)
            extra+=",\n  \"close_reason\": "+json_quote(data.at("close_reason").text);
        success=true;
    } catch(const std::exception& e) { error=e.what(); }
    std::string json="{\n  \"schema\": 1,\n  \"status\": "+json_quote(success?"complete":"failed")+
        ",\n  \"source_json\": "+json_quote(metadata.filename().string())+
        ",\n  \"source_video\": "+json_quote(source.filename().string())+
        ",\n  \"output_video\": "+json_quote(output.filename().string())+extra;
    if(!success) json+=",\n  \"error\": "+json_quote(error);
    publish(report,json+"\n}\n");
    std::cout << "[delivery] " << (success?"complete ":"failed ") << metadata.filename().string()
              << (success?"":": "+error) << '\n';
    return success?0:1;
}
int main(int argc,char** argv) {
    if(argc!=2) { std::cerr << "usage: camera_delivery EVENT.json\n"; return 2; }
    try { return finalize(argv[1]); }
    catch(const std::exception& e) { std::cerr << "[delivery] " << e.what() << '\n'; return 1; }
}
