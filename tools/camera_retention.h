#pragma once
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

namespace retention {
namespace fs = std::filesystem;
constexpr char marker[] = ".camera-retention-v1";
constexpr unsigned long long mib = 1024 * 1024;

inline unsigned setting(const char* name, unsigned fallback, unsigned maximum) {
    const char* raw = std::getenv(name);
    if (!raw) return fallback;
    std::string s(raw);
    if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != s.npos)
        throw std::runtime_error(std::string("invalid ") + name);
    unsigned n = std::stoul(s);
    if (n > maximum) throw std::runtime_error(std::string("out of range: ") + name);
    return n;
}
struct Policy {
    unsigned high = setting("CAMERA_MAX_MIB", 1024, 999999999);
    unsigned low = setting("CAMERA_TARGET_MIB", 768, 999999999);
    unsigned interval = setting("CAMERA_SWEEP_SEC", 30, 3600);
    Policy() {
        if (low >= high || !interval) throw std::runtime_error("invalid retention watermarks or interval");
    }
};
inline void private_dir(const fs::path& p) {
    struct stat st{};
    if (!p.is_absolute() || p != fs::canonical(p) || lstat(p.c_str(), &st) ||
        !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077))
        throw std::runtime_error("expected canonical private directory owned by service: " + p.string());
}
inline int lock(const fs::path& path) {
    int fd = open(path.c_str(), O_NOFOLLOW | O_CREAT | O_CLOEXEC | O_RDWR | O_NONBLOCK, 0600);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || flock(fd, LOCK_EX | LOCK_NB)) {
        if (fd >= 0) close(fd);
        throw std::runtime_error("cannot lock " + path.string());
    }
    return fd;
}
inline void sync_dir(const fs::path& p) {
    int fd = open(p.c_str(), O_DIRECTORY | O_CLOEXEC | O_RDONLY);
    bool ok = fd >= 0 && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    if (!ok) throw std::runtime_error("cannot sync directory " + p.string());
}
inline bool run_name(const fs::path& p) {
    static const std::regex pattern("run-[0-9]{8}");
    return std::regex_match(p.filename().string(), pattern);
}
inline bool enrolled(const fs::path& p) {
    if (!run_name(p) || !fs::is_directory(fs::symlink_status(p))) return false;
    struct stat st{};
    return lstat((p / marker).c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == geteuid() && st.st_nlink == 1 && st.st_size == 0;
}
inline fs::path next_run(const fs::path& root) {
    private_dir(root);
    unsigned last = 0;
    for (const auto& e : fs::directory_iterator(root))
        if (run_name(e.path())) last = std::max(last, static_cast<unsigned>(
            std::stoul(e.path().filename().string().substr(4))));
    if (last == 99999999) throw std::runtime_error("run numbers exhausted");
    char name[16];
    std::snprintf(name, sizeof(name), "run-%08u", last + 1);
    return root / name;
}
// The supervisor holds this descriptor until exit. Helpers never inherit it across exec.
inline int enroll(const fs::path& root, const fs::path& dir) {
    private_dir(root);
    private_dir(dir);
    if (dir.parent_path() != root || !run_name(dir))
        throw std::runtime_error("managed output must be ROOT/run-NNNNNNNN");
    int fd = lock(root / ".camera-owner.lock");
    try {
        for (const auto& e : fs::directory_iterator(root))
            if (enrolled(e.path()) && e.path().filename() >= dir.filename())
                throw std::runtime_error("allocate a fresh increasing run number");
        int m = open((dir / marker).c_str(), O_NOFOLLOW | O_EXCL | O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (m < 0) throw std::runtime_error("cannot enroll run");
        bool ok = fsync(m) == 0;
        close(m);
        if (!ok) throw std::runtime_error("cannot sync enrollment");
        sync_dir(dir);
        sync_dir(root);
        return fd;
    } catch (...) { close(fd); throw; }
}
} // namespace retention
