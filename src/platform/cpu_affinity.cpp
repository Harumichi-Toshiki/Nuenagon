// CPU の構成の調べ方と、スレッドの固定（設計書 §E2.4）。
#include "platform/cpu_affinity.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <fstream>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cpu {
namespace {

// 数値の列（"0,2,4-7"）を解く。範囲の外・書式の誤りは false
bool parse_list(const std::string& spec, std::vector<int>& out) {
    out.clear();
    size_t i = 0;
    while (i <= spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        const std::string item = spec.substr(i, j - i);
        if (item.empty()) return false;
        const size_t dash = item.find('-');
        auto number = [](const std::string& t, int& v) {
            if (t.empty()) return false;
            for (char c : t) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            }
            if (t.size() > 6) return false;
            v = std::stoi(t);
            return true;
        };
        int lo = 0, hi = 0;
        if (dash == std::string::npos) {
            if (!number(item, lo)) return false;
            hi = lo;
        }
        else {
            if (!number(item.substr(0, dash), lo) || !number(item.substr(dash + 1), hi) || hi < lo) return false;
        }
        for (int c = lo; c <= hi; c++) out.push_back(c);
        i = j + 1;
    }
    return !out.empty();
}

}  // namespace

#ifdef __linux__

Topology query_topology() {
    Topology t;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        for (int c = 0; c < CPU_SETSIZE; c++) {
            if (CPU_ISSET(c, &set)) t.allowed.push_back(c);
        }
    }
    const long configured = sysconf(_SC_NPROCESSORS_CONF);
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    t.logical = online > 0 ? (int)online : (int)t.allowed.size();

    // 物理コア：topology の（パッケージ番号, コア番号）で数える。読めない CPU は、それだけで1つのコアとする
    std::map<std::pair<long, long>, int> ids;
    const int n = configured > 0 ? (int)configured : 0;
    t.core_of.assign(n, 0);
    for (int c = 0; c < n; c++) {
        const std::string dir = "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/topology/";
        std::ifstream fp(dir + "physical_package_id"), fc(dir + "core_id");
        long pkg = -1, core = -1;
        std::pair<long, long> key;
        if (fp >> pkg && fc >> core) key = {pkg, core};
        else key = {-1 - c, 0};
        auto it = ids.find(key);
        if (it == ids.end()) it = ids.emplace(key, (int)ids.size()).first;
        t.core_of[c] = it->second;
    }
    t.physical = (int)ids.size();
    if (t.physical == 0) t.physical = t.logical;
    return t;
}

bool pin_current_thread(int cpu_id, std::string& err) {
    if (cpu_id < 0 || cpu_id >= CPU_SETSIZE) {
        err = "論理CPU " + std::to_string(cpu_id) + " は指定できない";
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    const int r = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    if (r != 0) {
        err = "論理CPU " + std::to_string(cpu_id) + " に固定できない（" + std::strerror(r) + "）";
        return false;
    }
    return true;
}

#elif defined(_WIN32)

Topology query_topology() {
    Topology t;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<char> buf(len > 0 ? len : 1);
    auto* base = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (len == 0 || !GetLogicalProcessorInformationEx(RelationProcessorCore, base, &len)) {
        // 数えられなければ、論理CPUの数だけを使う（1コア1CPU とみなす）
        t.logical = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        t.physical = t.logical;
        for (int c = 0; c < t.logical; c++) {
            t.allowed.push_back(c);
            t.core_of.push_back(c);
        }
        return t;
    }
    // プロセッサグループ（64 CPU ずつ）の番号を、論理CPU番号（group * 64 + bit）にする
    int cores = 0;
    for (char* p = buf.data(); p < buf.data() + len;) {
        auto* rec = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
        if (rec->Relationship == RelationProcessorCore) {
            const int core = cores++;
            for (WORD g = 0; g < rec->Processor.GroupCount; g++) {
                const GROUP_AFFINITY& ga = rec->Processor.GroupMask[g];
                for (int bit = 0; bit < 64; bit++) {
                    if (!(ga.Mask & ((KAFFINITY)1 << bit))) continue;
                    const int c = (int)ga.Group * 64 + bit;
                    if ((int)t.core_of.size() <= c) t.core_of.resize(c + 1, -1);
                    t.core_of[c] = core;
                    t.allowed.push_back(c);
                }
            }
        }
        p += rec->Size;
    }
    std::sort(t.allowed.begin(), t.allowed.end());
    t.logical = (int)t.allowed.size();
    t.physical = cores;
    return t;
}

bool pin_current_thread(int cpu_id, std::string& err) {
    if (cpu_id < 0) {
        err = "論理CPU " + std::to_string(cpu_id) + " は指定できない";
        return false;
    }
    GROUP_AFFINITY ga;
    std::memset(&ga, 0, sizeof ga);
    ga.Group = (WORD)(cpu_id / 64);
    ga.Mask = (KAFFINITY)1 << (cpu_id % 64);
    GROUP_AFFINITY prev;
    if (!SetThreadGroupAffinity(GetCurrentThread(), &ga, &prev)) {
        err = "論理CPU " + std::to_string(cpu_id) + " に固定できない（エラー " + std::to_string(GetLastError()) + "）";
        return false;
    }
    return true;
}

#else

Topology query_topology() {
    Topology t;
    t.logical = 1;
    t.physical = 1;
    t.allowed = {0};
    t.core_of = {0};
    return t;
}

bool pin_current_thread(int, std::string& err) {
    err = "この OS では CPU の固定に対応していない";
    return false;
}

#endif

bool select_cpus(const std::string& spec, bool physical_only, const Topology& topo,
                 std::vector<int>& out, std::string& err) {
    out.clear();
    std::vector<int> cand;
    std::string s = spec;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    if (s == "auto" || s.empty()) {
        cand = topo.allowed;
    }
    else {
        if (!parse_list(spec, cand)) {
            err = "CpuAffinity の値が読めない（" + spec + "）。auto として扱う";
            cand = topo.allowed;
        }
        else {
            std::sort(cand.begin(), cand.end());
            cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
            for (int c : cand) {
                if (c >= (int)topo.core_of.size() || topo.core_of[c] < 0) {
                    err = "論理CPU " + std::to_string(c) + " は無い";
                    return false;
                }
            }
        }
    }
    if (cand.empty()) {
        err = "使える論理CPUが無い";
        return false;
    }
    if (!physical_only) {
        out = cand;
        return true;
    }
    // 物理コアごとに、最も小さい番号の論理CPUを1つ選ぶ
    std::set<int> seen;
    for (int c : cand) {
        const int core = topo.core_of[c];
        if (seen.insert(core).second) out.push_back(c);
    }
    return true;
}

}  // namespace cpu
