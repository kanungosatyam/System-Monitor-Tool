// src/main.cpp
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>

using namespace std::chrono_literals;

struct ProcStat {
    int pid;
    std::string name;
    unsigned long utime;
    unsigned long stime;
    unsigned long total_time() const { return utime + stime; }
    long rss_pages; // resident set size in pages
    double cpu_percent{0.0};
    double mem_percent{0.0};
};

static long page_size_kb() {
    static long p = sysconf(_SC_PAGESIZE) / 1024;
    return p;
}

static unsigned long get_total_cpu_time() {
    std::ifstream f("/proc/stat");
    std::string line;
    std::getline(f, line);
    // line starts with "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
    std::istringstream ss(line);
    std::string cpu;
    ss >> cpu;
    unsigned long value, total = 0;
    while (ss >> value) total += value;
    return total;
}

static unsigned long get_uptime_ticks() {
    std::ifstream f("/proc/uptime");
    double up;
    f >> up;
    return static_cast<unsigned long>(up);
}

static std::map<int, ProcStat> read_proc_stats() {
    std::map<int, ProcStat> procs;
    DIR* dir = opendir("/proc");
    if (!dir) return procs;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        std::string dname = entry->d_name;
        if (!std::all_of(dname.begin(), dname.end(), ::isdigit)) continue;
        int pid = std::stoi(dname);
        ProcStat ps;
        ps.pid = pid;
        // read /proc/<pid>/stat
        std::string statPath = "/proc/" + dname + "/stat";
        std::ifstream s(statPath);
        if (!s) continue;
        std::string rest;
        s >> rest; // pid
        // next token is command in parentheses which may contain spaces
        std::string comm;
        std::getline(s, comm, ')'); // read until ')'
        if (!comm.empty() && comm.front() == ' ') comm = comm.substr(2); // remove " ("
        ps.name = comm;
        // after comm there are many fields; we need utime (14), stime (15), rss (24)
        // seek back and parse differently
        s.clear();
        s.seekg(0);
        std::string tmp;
        std::vector<std::string> fields;
        while (s >> tmp) fields.push_back(tmp);
        if (fields.size() >= 24) {
            // utime is at index 13, stime 14, rss 23 (0-based)
            ps.utime = std::stoul(fields[13]);
            ps.stime = std::stoul(fields[14]);
            ps.rss_pages = std::stol(fields[23]);
            procs[pid] = ps;
        }
    }
    closedir(dir);
    return procs;
}

static unsigned long get_mem_total_kb() {
    std::ifstream f("/proc/meminfo");
    std::string key;
    unsigned long val;
    std::string unit;
    while (f >> key >> val >> unit) {
        if (key == "MemTotal:") return val; // in kB
    }
    return 1;
}

void clear_screen() {
    std::cout << "\033[2J\033[H"; // ANSI clear and move cursor to home
}

int main() {
    const int refresh_ms = 2000;
    unsigned long prev_total_cpu = get_total_cpu_time();
    auto prev_procs = read_proc_stats();
    unsigned long mem_total_kb = get_mem_total_kb();
    long page_kb = page_size_kb();

    bool sort_by_cpu = true;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(refresh_ms));
        unsigned long cur_total_cpu = get_total_cpu_time();
        auto cur_procs = read_proc_stats();

        unsigned long total_cpu_delta = cur_total_cpu - prev_total_cpu;
        if (total_cpu_delta == 0) total_cpu_delta = 1;

        std::vector<ProcStat> list;
        for (auto &kv : cur_procs) {
            int pid = kv.first;
            ProcStat cur = kv.second;
            auto it = prev_procs.find(pid);
            if (it != prev_procs.end()) {
                unsigned long prev_total = it->second.total_time();
                unsigned long cur_total = cur.total_time();
                unsigned long proc_delta = (cur_total > prev_total) ? (cur_total - prev_total) : 0;
                cur.cpu_percent = 100.0 * proc_delta / total_cpu_delta;
            } else {
                cur.cpu_percent = 0.0;
            }
            cur.mem_percent = 100.0 * (cur.rss_pages * page_kb) / mem_total_kb;
            list.push_back(cur);
        }

        if (sort_by_cpu) {
            std::sort(list.begin(), list.end(), [](const ProcStat&a, const ProcStat&b){
                return a.cpu_percent > b.cpu_percent;
            });
        } else {
            std::sort(list.begin(), list.end(), [](const ProcStat&a, const ProcStat&b){
                return a.mem_percent > b.mem_percent;
            });
        }

        clear_screen();

        // header
	std::cout << "==============================\n";
	std::cout << "   SYSTEM MONITOR TOOL (LSP)\n";
	std::cout << "==============================\n\n";

	std::cout << "\033[1;32mSystem Monitor - refresh every 2s\033[0m\n";
        std::cout << "Sort by: " << (sort_by_cpu ? "CPU" : "MEM") << "    (press 's' then [ENTER] to toggle; 'k' to kill; 'q' to quit)\n\n";

        // system summary
        unsigned long total_cur = cur_total_cpu;
        unsigned long total_prev = prev_total_cpu;
        unsigned long sys_delta = (total_cur - total_prev);
        double sys_cpu_usage = 0.0;
        if (sys_delta > 0) {
            // approx CPU busy fraction from /proc/stat values is already part of get_total_cpu_time
            // We will simply show an approximate by using 1 - idle/total. But to keep simple, show blank place.
            sys_cpu_usage = 0.0;
        }
        // Mem info
        std::ifstream mf("/proc/meminfo");
        std::string k;
        unsigned long v;
        std::string u;
        unsigned long mem_free = 0, mem_available = 0;
        mf.clear();
        mf.seekg(0);
        while (mf >> k >> v >> u) {
            if (k == "MemAvailable:") { mem_available = v; break; }
        }
        unsigned long used_kb = (mem_total_kb - mem_available);
        double mem_percent_total = 100.0 * used_kb / mem_total_kb;
        std::cout << "Memory: " << used_kb/1024 << " MB / " << mem_total_kb/1024 << " MB (" << std::fixed << std::setprecision(1) << mem_percent_total << "%)\n\n";

        // table header
        std::cout << std::left << std::setw(8) << "PID" << std::setw(24) << "NAME" 
                  << std::right << std::setw(8) << "CPU%" << std::setw(10) << "MEM%" << "\n";
        std::cout << std::string(50, '-') << "\n";

        int shown = 0;
        for (auto &p : list) {
            if (shown++ >= 20) break; // show top 20
            std::cout << std::left << std::setw(8) << p.pid << std::setw(24) << (p.name.size() > 22 ? p.name.substr(0,22) : p.name)
                      << std::right << std::setw(8) << std::fixed << std::setprecision(2) << p.cpu_percent
                      << std::setw(10) << std::fixed << std::setprecision(2) << p.mem_percent << "\n";
        }

        // interactive: look for user input without blocking
        std::cout << "\nCommand: (s=toggle sort, k=kill pid, q=quit) > " << std::flush;
        // set non-blocking read using select on stdin
        fd_set set;
        struct timeval tv;
        FD_ZERO(&set);
        FD_SET(STDIN_FILENO, &set);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        int rv = select(STDIN_FILENO+1, &set, NULL, NULL, &tv);
        if (rv > 0 && FD_ISSET(STDIN_FILENO, &set)) {
            std::string cmd;
            std::getline(std::cin, cmd);
            if (cmd == "s") {
                sort_by_cpu = !sort_by_cpu;
            } else if (cmd == "q") {
                break;
            } else if (!cmd.empty() && cmd[0] == 'k') {
                // expecting: k <pid>
                std::istringstream iss(cmd);
                char c; int pid;
                iss >> c >> pid;
                if (pid > 0) {
                    std::cout << "Confirm kill " << pid << " ? (y/n): " << std::flush;
                    std::string r;
                    std::getline(std::cin, r);
                    if (!r.empty() && (r[0] == 'y' || r[0] == 'Y')) {
                        if (kill(pid, SIGKILL) == 0) {
                            std::cout << "Sent SIGKILL to " << pid << "\n";
                        } else {
                            std::perror("kill");
                        }
                        std::this_thread::sleep_for(800ms);
                    }
                }
            } else if (!cmd.empty() && std::all_of(cmd.begin(), cmd.end(), ::isdigit)) {
                // allow just entering 123 to be treated as kill prompt
                int pid = std::stoi(cmd);
                std::cout << "Confirm kill " << pid << " ? (y/n): " << std::flush;
                std::string r;
                std::getline(std::cin, r);
                if (!r.empty() && (r[0] == 'y' || r[0] == 'Y')) {
                    if (kill(pid, SIGKILL) == 0) {
                        std::cout << "Sent SIGKILL to " << pid << "\n";
                    } else {
                        std::perror("kill");
                    }
                    std::this_thread::sleep_for(800ms);
                }
            } else {
                // unknown command
            }
        }

        prev_total_cpu = cur_total_cpu;
        prev_procs = std::move(cur_procs);
    }

    return 0;
}
