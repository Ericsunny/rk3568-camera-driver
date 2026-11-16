// Start and supervise two independent camera services.
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
static volatile sig_atomic_t requested_signal = 0;
static void on_signal(int sig) { requested_signal = sig; }

struct Child {
    const char *name;
    const char *profile;
    int port;
    pid_t pid = -1;
    int status = 0;
    bool running = false;
};

static bool parse_port(const char *text, int &value) {
    char *end = nullptr;
    errno = 0;
    long n = std::strtol(text, &end, 10);
    if (errno || !*text || *end || n < 1 || n > 65535) return false;
    value = static_cast<int>(n);
    return true;
}

static int exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return 1;
}

static void stop_children(Child (&children)[2]) {
    for (auto &child : children) {
        if (child.running && kill(child.pid, SIGTERM) < 0 && errno != ESRCH)
            std::cerr << "cannot stop " << child.name << ": " << strerror(errno) << '\n';
    }
}

int main(int argc, char **argv) {
    if (argc < 3 || (std::string(argv[1]) != "manual" && std::string(argv[1]) != "detect")) {
        std::cerr << "usage: run_dual_camera manual|detect ABSOLUTE_NEW_DIRECTORY [--imx-port N] [--ov-port N]\n";
        return 2;
    }
    fs::path run_dir(argv[2]);
    if (!run_dir.is_absolute()) { std::cerr << "run directory must be absolute\n"; return 2; }
    Child children[2] = {{"imx415", "imx415-isp", 8554}, {"ov8858", "ov8858-isp", 8555}};
    fs::path wrapper = fs::canonical("/proc/self/exe").parent_path() / "camera_demo";
    // Explicit injection point for isolated host tests.
    if (const char *p = std::getenv("CAMERA_DEMO_WRAPPER")) wrapper = p;
    int timeout = 150;
    if (const char *p = std::getenv("DUAL_SHUTDOWN_TIMEOUT")) {
        if (!parse_port(p, timeout)) { std::cerr << "invalid shutdown timeout\n"; return 2; }
    }
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc) { std::cerr << "missing option value\n"; return 2; }
        int *port = nullptr;
        if (std::string(argv[i]) == "--imx-port") port = &children[0].port;
        else if (std::string(argv[i]) == "--ov-port") port = &children[1].port;
        if (!port || !parse_port(argv[i + 1], *port)) { std::cerr << "invalid option or port\n"; return 2; }
    }
    if (children[0].port == children[1].port) { std::cerr << "ports must be distinct\n"; return 2; }
    if (mkdir(run_dir.c_str(), 0700) != 0) {
        std::cerr << "cannot create run directory: " << strerror(errno) << '\n'; return 1;
    }
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    std::string error;
    for (auto &child : children) {
        if (requested_signal) break;
        fs::path log = run_dir / (std::string(child.name) + ".log");
        int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) { error = "cannot create child log"; break; }
        pid_t pid = fork();
        if (pid == 0) {
            if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(127);
            close(fd);
            std::string port = std::to_string(child.port);
            if (setenv("RTSP_PORT_OVERRIDE", port.c_str(), 1) < 0) _exit(127);
            fs::path output = run_dir / child.name;
            execl(wrapper.c_str(), wrapper.c_str(), child.profile, argv[1], output.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        close(fd);
        if (pid < 0) { error = "cannot fork child"; break; }
        child.pid = pid;
        child.running = true;
    }
    bool stopping = false;
    auto stop_at = std::chrono::steady_clock::now();
    for (;;) {
        int alive = 0;
        for (auto &child : children) {
            if (!child.running) continue;
            int status = 0;
            pid_t got = waitpid(child.pid, &status, WNOHANG);
            if (got == child.pid) { child.running = false; child.status = status; }
            else if (got < 0 && errno != EINTR) { child.running = false; error = "waitpid failed"; }
            if (child.running) ++alive;
        }
        if (!stopping && (requested_signal || !error.empty() || alive < 2)) {
            stopping = true; stop_at = std::chrono::steady_clock::now(); stop_children(children);
        }
        if (!alive) break;
        if (stopping && std::chrono::steady_clock::now() - stop_at > std::chrono::seconds(timeout)) {
            error = "shutdown timeout; inspect remaining processes and devices";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    bool ok = requested_signal && error.empty();
    for (const auto &child : children)
        ok = ok && child.pid > 0 && !child.running && exit_code(child.status) == 0;
    std::ofstream out(run_dir / "status.json");
    if (!out) { std::cerr << "cannot write status.json\n"; return 1; }
    out << "{\n  \"status\": \"" << (ok ? "stopped" : "failed")
        << "\",\n  \"signal\": ";
    if (requested_signal) out << requested_signal; else out << "null";
    out << ",\n  \"mode\": \"" << argv[1] << "\",\n  \"ports\": {\"imx415\": "
        << children[0].port << ", \"ov8858\": " << children[1].port << "},\n  \"children\": {";
    bool first = true;
    for (const auto &child : children) {
        if (child.pid <= 0) continue;
        if (!first) out << ", ";
        first = false;
        out << "\"" << child.name << "\": {\"pid\": " << child.pid << ", \"returncode\": ";
        if (child.running) out << "null"; else out << exit_code(child.status);
        out << "}";
    }
    out << "},\n  \"error\": ";
    if (error.empty()) out << "null"; else out << '"' << error << '"';
    out << "\n}\n";
    out.close();
    if (!out) { std::cerr << "cannot finish status.json\n"; return 1; }
    return ok ? 0 : 1;
}
