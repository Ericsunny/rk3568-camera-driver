// One online archive maintenance pass. No camera, encoder or delivery handles.
#include "camera_retention.h"
#include "camera_json.h"
#include "camera_process.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <sys/statvfs.h>

namespace fs = std::filesystem;
using namespace retention;
static const std::set<std::string> final_files{
    ".json", ".h264", ".mp4", ".packets.csv", ".delivery.json"};
struct Event {
    fs::path base;
    unsigned long serial, segment;
    std::set<std::string> files;
    uintmax_t blocks = 0;
    bool regular = true;
    fs::path path(const std::string& suffix) const { return base.string() + suffix; }
};
static uintmax_t available(const fs::path& root) {
    struct statvfs v{};
    if (statvfs(root.c_str(), &v)) throw std::runtime_error("statvfs failed");
    return uintmax_t(v.f_bavail) * v.f_frsize;
}
static bool present(const fs::path& path) { return fs::exists(fs::symlink_status(path)); }
static bool allowed(const Event& e) {
    return !present(e.base.parent_path() / ".keep") && !present(e.path(".keep"));
}
static bool verify(const Event& e, bool resuming = false) {
    if (!e.regular || !allowed(e)) return false;
    if (resuming) {
        auto remaining = e.files;
        remaining.erase(".retiring");
        if (remaining.count(".delivery.json") ||
            !std::includes(final_files.begin(),final_files.end(),remaining.begin(),remaining.end()))
            return false;
    } else if (e.files != final_files) return false;
    auto receipt = e.path(resuming ? ".retiring" : ".delivery.json");
    if (fs::file_size(receipt) > 65536) return false;
    std::ifstream in(receipt);
    auto j = camera::parse_json(std::string(std::istreambuf_iterator<char>(in), {}));
    auto matches = [&](const char* key, const std::string& wanted) {
        const auto& v = j.at(key);
        return v.type == camera::Json::String && v.text == wanted;
    };
    if (!matches("status","complete") || j.at("schema").number() != 1 ||
        !matches("source_json",e.path(".json").filename().string()) ||
        !matches("source_video",e.path(".h264").filename().string()) ||
        !matches("output_video",e.path(".mp4").filename().string())) return false;
    for (auto [suffix, key] : {std::pair{".json","metadata_sha256"}, {".h264","source_sha256"},
                              {".mp4","output_sha256"}, {".packets.csv","packet_log_sha256"}}) {
        if (resuming && !e.files.count(suffix)) continue;
        auto sum = camera::checked({"sha256sum","--",e.path(suffix).string()}, 60);
        if (sum.size() < 64 || !matches(key,sum.substr(0,64))) return false;
    }
    return allowed(e);
}
// The durable receipt rename marks the event as retiring. Persist payload
// unlinks before removing the intent, so a restart can finish a partial reclaim.
static void finish(const Event& e) {
    for (const char* ext : {".json",".h264",".mp4",".packets.csv"})
        fs::remove(e.path(ext));
    sync_dir(e.base.parent_path());
    fs::remove(e.path(".retiring"));
    sync_dir(e.base.parent_path());
}
static void sweep(const fs::path& root, const Policy& cfg) {
    static const std::regex name(R"(^(event_[0-9]{8}-[0-9]{6}-([0-9]{1,10})(?:-s([0-9]{1,10}))?)(\..+)$)");
    std::map<fs::path, Event> events;
    uintmax_t used = 0;
    struct stat volume{};
    if (stat(root.c_str(), &volume)) throw std::runtime_error("cannot stat archive root");
    for (const auto& dir : fs::directory_iterator(root)) {
        if (!enrolled(dir.path())) continue;
        private_dir(dir.path());
        struct stat run{};
        if (stat(dir.path().c_str(), &run) || run.st_dev != volume.st_dev)
            throw std::runtime_error("managed run must be on the archive filesystem");
        for (const auto& file : fs::directory_iterator(dir.path())) {
            struct stat st{};
            if (lstat(file.path().c_str(), &st)) {
                if (errno == ENOENT) continue; // delivery published a new name during enumeration
                throw std::runtime_error("cannot stat archive entry");
            }
            uintmax_t size = uintmax_t(st.st_blocks) * 512;
            used += size;
            std::smatch m;
            auto filename = file.path().filename().string();
            if (!std::regex_match(filename,m,name)) continue;
            auto base = dir.path() / m[1].str();
            auto& e = events[base];
            e.base = base;
            e.serial = std::stoul(m[2]);
            e.segment = m[3].matched ? std::stoul(m[3]) : 0;
            e.regular &= S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_dev == volume.st_dev;
            e.files.insert(m[4]);
            e.blocks += size;
        }
    }
    std::vector<const Event*> order;
    for (const auto& item : events) order.push_back(&item.second);
    std::sort(order.begin(),order.end(),[](const Event* a,const Event* b) {
        return std::make_tuple(a->base.parent_path(),a->serial,a->segment) <
               std::make_tuple(b->base.parent_path(),b->serial,b->segment);
    });
    for (const auto* e : order) {
        if (!e->files.count(".retiring")) continue;
        try { if (!verify(*e,true)) continue; }
        catch (const std::exception& ex) { std::cerr << "[retention] resume " << ex.what() << '\n'; continue; }
        finish(*e);
        used -= e->blocks;
        std::cerr << "[retention] resumed " << e->base << '\n';
    }
    auto free_bytes = available(root);
    if (used < cfg.high*mib && free_bytes >= 1200*mib) return;
    for (const auto* e : order) {
        if (used <= cfg.low*mib && free_bytes >= 2100*mib) return;
        if (e->files.count(".retiring")) continue;
        bool verified = false;
        try { verified = verify(*e); }
        catch (const std::exception& ex) { std::cerr << "[retention] verify " << e->base << ' ' << ex.what() << '\n'; }
        if (!verified) continue;
        fs::rename(e->path(".delivery.json"),e->path(".retiring"));
        sync_dir(e->base.parent_path());
        finish(*e);
        used -= e->blocks;
        free_bytes = available(root);
        std::cerr << "[retention] reclaimed " << e->base << " allocated_bytes=" << e->blocks << '\n';
    }
    if (used > cfg.low*mib || available(root) < 2100*mib)
        std::cerr << "[retention] target unmet; protected or unowned data remains, managed_bytes=" << used << '\n';
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        fs::path root(argv[1]);
        private_dir(root);
        int fd = retention::lock(root / ".camera-sweep.lock");
        errno = 0;
        if (nice(10) == -1 && errno) throw std::runtime_error("cannot lower cleaner priority");
        sweep(root, Policy{});
        close(fd);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[retention] " << e.what() << '\n';
        return 1;
    }
}
