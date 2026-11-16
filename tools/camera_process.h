// POSIX child-process helpers for the camera service.
#ifndef CAMERA_PROCESS_H
#define CAMERA_PROCESS_H
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace camera {
using Clock = std::chrono::steady_clock;
inline int status_code(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) :
           WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}
inline pid_t spawn(const std::vector<std::string>& args, int out = -1,
                   int err = -1, bool group = false) {
    if (args.empty()) throw std::runtime_error("empty command");
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (!pid) {
        if (group && setpgid(0, 0)) _exit(127);
        if ((out >= 0 && dup2(out, STDOUT_FILENO) < 0) ||
            (err >= 0 && dup2(err, STDERR_FILENO) < 0)) _exit(127);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}
struct Result { int status; std::string out, err; };
inline std::string read_temp(FILE *f) {
    if (fflush(f) || fseek(f, 0, SEEK_SET)) throw std::runtime_error("temporary output seek failed");
    std::string s; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f))) {
        s.append(buf, n);
        if (s.size() > 4 * 1024 * 1024) throw std::runtime_error("command output too large");
    }
    if (ferror(f)) throw std::runtime_error("temporary output read failed");
    return s;
}
inline Result run(const std::vector<std::string>& args, int seconds = 120) {
    FILE *out = tmpfile(), *err = tmpfile();
    if (!out || !err) {
        if (out) fclose(out);
        if (err) fclose(err);
        throw std::runtime_error("cannot create temporary output");
    }
    pid_t pid = -1;
    try {
        // Inherit the supervisor's process group so a timed-out delivery
        // worker and its ffmpeg children can be terminated together.
        pid = spawn(args, fileno(out), fileno(err), false);
        int st = 0;
        auto until = Clock::now() + std::chrono::seconds(seconds);
        for (;;) {
            pid_t done = waitpid(pid, &st, WNOHANG);
            if (done == pid) break;
            if (done < 0 && errno != EINTR) throw std::runtime_error("waitpid failed");
            if (Clock::now() >= until) {
                kill(pid, SIGKILL);
                auto reap_until = Clock::now() + std::chrono::seconds(2);
                bool reaped = false;
                while (Clock::now() < reap_until) {
                    auto got = waitpid(pid, &st, WNOHANG);
                    if (got == pid || (got < 0 && errno == ECHILD)) { reaped = true; break; }
                    if (got < 0 && errno != EINTR) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                auto old_pid = pid;
                pid = -1;
                throw std::runtime_error(reaped ? "command timed out" :
                                         "timed-out command still alive: " + std::to_string(old_pid));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        pid = -1;
        Result r{status_code(st), read_temp(out), read_temp(err)};
        fclose(out); fclose(err);
        return r;
    } catch (...) {
        if (pid > 0) {
            kill(pid, SIGKILL);
            (void)waitpid(pid, nullptr, WNOHANG);
        }
        fclose(out); fclose(err); throw;
    }
}
inline std::string checked(const std::vector<std::string>& args, int seconds = 120) {
    auto r = run(args, seconds);
    if (r.status) throw std::runtime_error(args[0] + " failed: " + r.err);
    return r.out;
}
}
#endif
