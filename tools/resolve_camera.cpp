// Resolve an ISP mainpath only through an enabled sensor-to-capture graph.
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <glob.h>
#include <iostream>
#include <map>
#include <poll.h>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct Graph {
    std::string media, driver, model;
    std::map<std::string, std::string> nodes;
    std::map<std::string, std::set<std::string>> edges;
};

static Graph parse_graph(const std::string &media, const std::string &text) {
    const std::regex header(R"(^\s*(driver|model)\s+(\S+))");
    const std::regex entity(R"(^- entity [0-9]+: (.*?) \()");
    const std::regex node(R"(device node name\s+(/dev/\S+))");
    const std::regex link(R"re(-> "([^"]+)":\d+ \[([^\]]+)\])re");
    Graph graph; graph.media = media;
    std::istringstream input(text);
    std::string line, current;
    std::smatch match;
    while (std::getline(input, line)) {
        if (std::regex_search(line, match, header)) {
            auto &field = (match[1] == "driver" ? graph.driver : graph.model);
            if (field.empty()) field = match[2];
        } else if (std::regex_search(line, match, entity)) {
            current = match[1]; graph.nodes[current] = ""; graph.edges[current];
        } else if (!current.empty()) {
            if (std::regex_search(line, match, node)) graph.nodes[current] = match[1];
            if (std::regex_search(line, match, link) && match[2].str().find("ENABLED") != std::string::npos)
                graph.edges[current].insert(match[1]);
        }
    }
    return graph;
}

static std::set<std::string> reachable(const Graph &graph, const std::vector<std::string> &starts) {
    std::set<std::string> visited(starts.begin(), starts.end());
    std::vector<std::string> queue(starts);
    for (size_t i = 0; i < queue.size(); ++i) {
        auto links = graph.edges.find(queue[i]);
        if (links == graph.edges.end()) continue;
        for (const auto &next : links->second)
            if (visited.insert(next).second) queue.push_back(next);
    }
    return visited;
}

static std::vector<std::string> sensor_starts(const Graph &graph, const std::string &sensor) {
    std::vector<std::string> starts;
    for (const auto &node : graph.nodes) {
        std::string lower = node.first;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower.find(sensor) != std::string::npos) starts.push_back(node.first);
    }
    return starts;
}

static bool query(const std::string &media, std::string &text) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        execlp("media-ctl", "media-ctl", "-d", media.c_str(), "-p", static_cast<char *>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return false; }
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool eof = false, failed = false;
    while (!eof && !failed) {
        if (std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
        pollfd pfd{fds[0], POLLIN | POLLHUP, 0};
        int ready = poll(&pfd, 1, 100);
        if (ready < 0 && errno != EINTR) { failed = true; break; }
        if (ready > 0) {
            char buffer[4096];
            ssize_t n = read(fds[0], buffer, sizeof(buffer));
            if (n > 0) { text.append(buffer, static_cast<size_t>(n)); if (text.size() > 4*1024*1024) failed = true; }
            else if (n == 0) eof = true;
            else if (errno != EAGAIN && errno != EINTR) failed = true;
        }
    }
    close(fds[0]);
    int status = 0;
    while (!failed) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done < 0 && errno != EINTR) return false;
        if (std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return false;
    return !failed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || (std::string(argv[1]) != "imx415-isp" && std::string(argv[1]) != "ov8858-isp")) {
        std::cerr << "usage: resolve_camera PROFILE [--selfpath] [/dev/mediaN ...]\n"; return 2;
    }
    const std::string profile(argv[1]);
    const std::string driver = profile == "imx415-isp" ? "rkisp-vir0" : "rkisp-vir1";
    const std::string sensor = profile == "imx415-isp" ? "imx415" : "ov8858";
    std::vector<std::string> media;
    bool side = argc > 2 && std::string(argv[2]) == "--selfpath";
    if (side && profile != "imx415-isp") return 2;
    const std::string entity = side ? "rkisp_selfpath" : "rkisp_mainpath";
    int first = side ? 3 : 2;
    if (argc > first) {
        for (int i = first; i < argc; ++i) media.emplace_back(argv[i]);
    } else {
        glob_t paths{};
        if (glob("/dev/media[0-9]*", 0, nullptr, &paths) == 0)
            for (size_t i = 0; i < paths.gl_pathc; ++i) media.emplace_back(paths.gl_pathv[i]);
        globfree(&paths);
    }
    std::vector<Graph> graphs;
    for (const auto &path : media) {
        std::string text;
        if (!query(path, text)) { std::cerr << "cannot query " << path << '\n'; return 1; }
        graphs.push_back(parse_graph(path, text));
    }
    std::vector<std::pair<std::string, std::string>> candidates;
    for (const auto &graph : graphs) {
        if (graph.driver != driver) continue;
        auto starts = sensor_starts(graph, sensor);
        if (profile == "ov8858-isp" && starts.empty()) {
            std::vector<std::string> bridges;
            for (const auto &up : graphs) {
                if (up.driver != "rkcif" || up.model.empty() || !graph.nodes.count(up.model)) continue;
                auto path = reachable(up, sensor_starts(up, sensor));
                if (path.count("rockchip-mipi-csi2") && path.count("stream_cif_mipi_id0")) bridges.push_back(up.model);
            }
            if (bridges.size() == 1) starts = bridges;
        }
        auto path = reachable(graph, starts);
        auto node = graph.nodes.find(entity);
        if (path.count(entity) && node != graph.nodes.end() && !node->second.empty())
            candidates.emplace_back(graph.media, node->second);
    }
    if (candidates.size() != 1) {
        std::cerr << "expected exactly one enabled path to " << entity << "; found " << candidates.size() << '\n'; return 1;
    }
    std::cerr << profile << ": " << candidates[0].first << " -> " << candidates[0].second << '\n';
    std::cout << candidates[0].second << '\n';
    return 0;
}
