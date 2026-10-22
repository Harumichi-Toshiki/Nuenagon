// CPU の構成の調べ方と、スレッドの固定（設計書 §E2.4）。Linux と Windows で処理を分ける（#ifdef）。
// 割り当てに失敗しても探索は続ける。呼び出し側は、失敗の理由を info string で出す。
#pragma once
#include <string>
#include <vector>

namespace cpu {

struct Topology {
    int logical = 0;               // 論理CPUの数
    int physical = 0;              // 物理コアの数
    std::vector<int> allowed;      // このプロセスが使ってよい論理CPU（昇順）
    std::vector<int> core_of;      // 論理CPU番号 → 物理コアの番号（0 から。同じコアの論理CPUは同じ値）
};

// 起動時に一度調べる（Linux：/sys/devices/system/cpu とアフィニティ、Windows：GetLogicalProcessorInformationEx）
Topology query_topology();

// CpuAffinity（"auto"、または "0,2,4-7" の形の論理CPUの一覧）と UsePhysicalCoresOnly から、
// 探索スレッドを固定する論理CPUの一覧を作る。昇順。physical_only のときは、物理コアごとに最も小さい番号の1つだけ。
// 指定が読めない・使える CPU が無いときは false と理由を err に入れる
bool select_cpus(const std::string& spec, bool physical_only, const Topology& topo,
                 std::vector<int>& out, std::string& err);

// 呼び出したスレッドを論理CPU cpu だけで動くように固定する。失敗なら false と理由を err に入れる
bool pin_current_thread(int cpu, std::string& err);

}  // namespace cpu
