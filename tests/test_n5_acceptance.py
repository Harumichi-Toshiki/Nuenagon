# 変更禁止（PROTECTED）。Nuenagon N5（高速化：探索の結果を変えずに速くする、CPU の割り当て）の受け入れテスト。
#
# ■ 結果が変わらないこと（設計書 v1.4 §E5.9・§E8）
#   tests/n5_cases.py の局面集（重みは3種類：material_v0、v1 の乱数の重み、v0 の極端な重み）を、
#   Threads=1・Deterministic=true・USI_Hash=64・go nodes N で探索したとき、すべての "info depth …" の行の
#   depth・seldepth・multipv・score・nodes・pv と、bestmove の行が、正解データ（golden/n5_golden.json）と一致する。
#   正解データは N4.1（コミット ae08a26）に「決定的モードではルートの詰み探索を時間で止めない」を加えたもので作った。
#   ただし、ルートの詰み探索が見つけた詰みの行（nodes が 0 の mate の行）は、手と「勝ちの詰み」であることだけを比べる
#   （詰み探索の手数は、標準ライブラリの実装の違いで変わることがあるため）。
#   これを、既定のビルド（build/）と、SIMD を使わないビルド（-DNUENAGON_SIMD=OFF、テストが build_scalar/ に作る）の両方で確かめる。
#
# ■ SIMD の有無の表示
#   isready の後に "info string Nuenagon build: simd=<avx2|scalar>" を出す（このビルドが実際に使う SIMD）。
#   CMake のオプション NUENAGON_SIMD（既定 ON）が ON なら、CPU が AVX2 に対応する環境で avx2、OFF なら scalar。
#
# ■ 速さ
#   テストが N4.1（コミット ae08a26）を一時フォルダにビルドし、同じ局面（tests/n5_cases.py の SPEED_POSITIONS）を
#   Threads=1・Deterministic=false・go nodes 300000 で、交互に2回ずつ探索する。各局面で速い方の時間を取り、
#   その合計で N4.1 ÷ 新しいエンジン が 2.0 以上であること。あわせて、両者の最後の info の nodes が一致すること
#   （同じ探索をしていること）。
#
# ■ CPU の割り当て（設計書 §E2.4。Linux で確かめる）
#   CpuAffinity=<番号> にすると、探索スレッドがその論理CPUだけで動く（/proc/<pid>/task/*/status の
#   Cpus_allowed_list が、その番号だけになる）。Threads=2 なら、そのスレッドが2つ以上ある。
#   UsePhysicalCoresOnly=true（CpuAffinity=auto）なら、探索スレッドはそれぞれ1つの論理CPUに固定され、
#   それらは互いに別の物理コアに属する。isready の後の "info string Nuenagon cpu: …" に physical=<物理コア数> を含める。
#
# ■ 詰み探索と stop
#   go infinite の直後（ルートの詰み探索の最中）に stop を送ったら、0.15 秒以内に bestmove を返す。
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import n5_cases as C  # noqa: E402

REF_COMMIT = "ae08a26"
GOLD = json.load(open(C.GOLDEN, encoding="utf-8"))


def exe_in(build_dir):
    for c in ("nuenagon", "Release/nuenagon.exe", "nuenagon.exe"):
        p = os.path.join(build_dir, c)
        if os.path.exists(p):
            return p
    raise FileNotFoundError(f"{build_dir} に nuenagon が無い")


def cmake_build(src, build_dir, extra=()):
    subprocess.run(["cmake", "-S", src, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release", *extra], check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", build_dir, "-j"], check=True, stdout=subprocess.DEVNULL)
    return exe_in(build_dir)


@pytest.fixture(scope="module")
def shk_paths(tmp_path_factory):
    d = tmp_path_factory.mktemp("n5shk")
    out = {}
    for kind, sha in GOLD["shk_sha256"].items():
        p = C.make_shk(kind, str(d / f"{kind}.shk"))
        got = C.sha256(p)
        assert got == sha, (f"重み {kind} の SHA-256 が正解データを作ったときと違う（{got}）。numpy の版の違いの可能性。"
                            "実装の問題ではないので、作業を止めて報告すること")
        out[kind] = p
    return out


def same_info(a, b):
    if a.get("nodes") == "0" and a.get("score", "").startswith("mate ") and not a["score"].startswith("mate -"):
        return (b.get("nodes") == "0" and b.get("score", "").startswith("mate ") and not b["score"].startswith("mate -")
                and a.get("pv", "").split()[:1] == b.get("pv", "").split()[:1])
    return a == b


def check_all(exe, shk_paths):
    engines = {}
    bad = []
    try:
        for case in GOLD["cases"]:
            kind = case["kind"]
            if kind not in engines:
                e = C.Engine(exe)
                C.setup(e, shk_paths[kind])
                engines[kind] = e
            r, _ = C.run_case(engines[kind], case["pos"], case["multipv"], case["nodes"])
            ok = r["bestmove"] == case["bestmove"] and len(r["infos"]) == len(case["infos"]) and \
                all(same_info(x, y) for x, y in zip(case["infos"], r["infos"]))
            if not ok:
                diff = next((i for i, (x, y) in enumerate(zip(case["infos"], r["infos"])) if not same_info(x, y)), None)
                detail = (f"info {diff} 行目：正解 {case['infos'][diff]} ／ 実際 {r['infos'][diff]}" if diff is not None else
                          f"info の行数 {len(case['infos'])} → {len(r['infos'])}、bestmove {case['bestmove']} → {r['bestmove']}")
                bad.append(f"{case['name']}（{case['pos'][:60]}）: {detail}")
    finally:
        for e in engines.values():
            e.close()
    assert not bad, f"{len(bad)} 局面で N4.1 と結果が違う:\n" + "\n".join(bad[:10])


def build_line(exe, shk):
    e = C.Engine(exe)
    try:
        e.send("usi")
        e.lines_until("usiok")
        e.send(f"setoption name EvalFile value {shk}")
        e.send("isready")
        lines = e.lines_until("readyok", timeout=300)
    finally:
        e.close()
    return lines


def has_avx2():
    try:
        return " avx2 " in (" " + open("/proc/cpuinfo").read().replace("\n", " ") + " ")
    except OSError:
        return False


# ---------------------------------------------------------------- 結果が変わらないこと

def test_same_results_default_build(shk_paths):
    check_all(exe_in(os.path.join(ROOT, "build")), shk_paths)


@pytest.fixture(scope="module")
def scalar_exe():
    return cmake_build(ROOT, os.path.join(ROOT, "build_scalar"), ["-DNUENAGON_SIMD=OFF"])


def test_same_results_scalar_build(scalar_exe, shk_paths):
    check_all(scalar_exe, shk_paths)


def test_simd_reported(scalar_exe):
    d = [x for x in build_line(exe_in(os.path.join(ROOT, "build")), C.MATERIAL) if "Nuenagon build:" in x]
    s = [x for x in build_line(scalar_exe, C.MATERIAL) if "Nuenagon build:" in x]
    assert d and s, "isready の後に 'info string Nuenagon build: simd=…' が出ていない"
    assert "simd=scalar" in s[0], f"NUENAGON_SIMD=OFF のビルドが scalar と表示していない: {s[0]}"
    if has_avx2():
        assert "simd=avx2" in d[0], f"既定のビルドが avx2 を使っていない: {d[0]}"


# ---------------------------------------------------------------- 速さ

@pytest.fixture(scope="module")
def ref_exe():
    tmp = tempfile.mkdtemp(prefix="nue_ref_")
    wt = os.path.join(tmp, "src")
    r = subprocess.run(["git", "-C", ROOT, "worktree", "add", "--detach", wt, REF_COMMIT], capture_output=True, text=True)
    assert r.returncode == 0, f"N4.1（{REF_COMMIT}）を取り出せない: {r.stderr}"
    try:
        yield cmake_build(wt, os.path.join(tmp, "build"))
    finally:
        subprocess.run(["git", "-C", ROOT, "worktree", "remove", "--force", wt], capture_output=True)
        shutil.rmtree(tmp, ignore_errors=True)


def test_speedup(ref_exe):
    new_exe = exe_in(os.path.join(ROOT, "build"))
    engines = {}
    for name, exe in (("ref", ref_exe), ("new", new_exe)):
        e = C.Engine(exe)
        C.setup(e, C.MATERIAL, deterministic=False)
        engines[name] = e
    best = {"ref": [], "new": []}
    nodes = {"ref": [], "new": []}
    try:
        for pos in C.SPEED_POSITIONS:
            t = {"ref": 1e9, "new": 1e9}
            for rep in range(2):
                for name in ("ref", "new"):
                    r, el = C.run_case(engines[name], pos, 1, 300000)
                    t[name] = min(t[name], el)
                    if rep == 0:
                        nodes[name].append(r["infos"][-1]["nodes"] if r["infos"] else None)
            for name in t:
                best[name].append(t[name])
    finally:
        for e in engines.values():
            e.close()
    rt, nt = sum(best["ref"]), sum(best["new"])
    ratio = rt / nt
    print(f"\n速さ：N4.1 {rt:.2f} 秒、新 {nt:.2f} 秒、{ratio:.2f} 倍（局面ごと：" +
          ", ".join(f"{a / b:.2f}" for a, b in zip(best['ref'], best['new'])) + "）")
    assert nodes["ref"] == nodes["new"], f"N4.1 と探索のノード数が違う（同じ探索になっていない）: {nodes}"
    assert ratio >= 2.0, f"N4.1 の {ratio:.2f} 倍（2.0 倍以上が必要）"


# ---------------------------------------------------------------- CPU の割り当て

def search_thread_cpus(pid):
    out = []
    for tid in os.listdir(f"/proc/{pid}/task"):
        try:
            st = open(f"/proc/{pid}/task/{tid}/status").read()
        except OSError:
            continue
        m = re.search(r"Cpus_allowed_list:\s*(\S+)", st)
        if m:
            out.append(m.group(1))
    return out


def with_infinite(opts, fn):
    e = C.Engine(exe_in(os.path.join(ROOT, "build")))
    try:
        e.send("usi")
        e.lines_until("usiok")
        for k, v in [("EvalFile", C.MATERIAL), ("NetworkDelay", 0)] + opts:
            e.send(f"setoption name {k} value {v}")
        e.send("isready")
        ready = e.lines_until("readyok", timeout=300)
        e.send("position startpos moves 7g7f 3c3d")
        e.send("go infinite")
        time.sleep(1.0)
        res = fn(e.p.pid, ready)
        e.send("stop")
        e.lines_until("bestmove", timeout=10)
        return res
    finally:
        e.close()


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="Linux の /proc で確かめる")
def test_cpu_affinity_pins_threads():
    ncpu = len(os.sched_getaffinity(0))
    assert ncpu >= 2, "このテストには論理CPUが2つ以上必要"
    target = sorted(os.sched_getaffinity(0))[-1]
    cpus = with_infinite([("Threads", 2), ("CpuAffinity", str(target))], lambda pid, _: search_thread_cpus(pid))
    pinned = [c for c in cpus if c == str(target)]
    assert len(pinned) >= 2, f"CpuAffinity={target}・Threads=2 で、その CPU だけで動くスレッドが {len(pinned)} 個（各スレッド: {cpus}）"


def physical_core_of(cpu):
    try:
        pkg = open(f"/sys/devices/system/cpu/cpu{cpu}/topology/physical_package_id").read().strip()
        core = open(f"/sys/devices/system/cpu/cpu{cpu}/topology/core_id").read().strip()
        return (pkg, core)
    except OSError:
        return ("?", str(cpu))


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="Linux の /proc で確かめる")
def test_physical_cores_only():
    allowed = sorted(os.sched_getaffinity(0))
    cores = {physical_core_of(c) for c in allowed}
    n = min(len(cores), 4)
    assert n >= 1
    res = with_infinite([("Threads", n), ("UsePhysicalCoresOnly", "true")],
                        lambda pid, ready: (search_thread_cpus(pid), ready))
    cpus, ready = res
    singles = [int(c) for c in cpus if re.fullmatch(r"\d+", c)]
    assert len(singles) >= n, f"Threads={n}・UsePhysicalCoresOnly で、1つの CPU に固定されたスレッドが {len(singles)} 個（{cpus}）"
    owners = [physical_core_of(c) for c in singles[:n]]
    if len(cores) >= n:
        assert len(set(owners)) == len(owners), f"同じ物理コアに2つのスレッドが固定されている: {singles} → {owners}"
    info = [x for x in ready if "Nuenagon cpu:" in x]
    assert info and re.search(r"physical=\d+", info[0]), f"cpu の info に physical= が無い: {info}"


# ---------------------------------------------------------------- 詰み探索と stop

def test_stop_during_root_mate_search():
    e = C.Engine(exe_in(os.path.join(ROOT, "build")))
    try:
        C.setup(e, C.MATERIAL, deterministic=False)
        worst = 0.0
        for sfen in C.MIDDLEGAME:
            e.send("usinewgame")
            e.send(C.pos_cmd(sfen))
            e.send("go infinite")
            time.sleep(0.02)
            t0 = time.time()
            e.send("stop")
            e.lines_until("bestmove", timeout=10)
            worst = max(worst, time.time() - t0)
        assert worst <= 0.15, f"go infinite の直後の stop から bestmove まで {worst:.3f} 秒（0.15 秒以内）"
    finally:
        e.close()


# ---------------------------------------------------------------- 参考の記録（合否には使わない）

def test_report_evalbench(capsys):
    e = C.Engine(exe_in(os.path.join(ROOT, "build")))
    try:
        C.setup(e, C.MATERIAL, deterministic=False)
        e.send(C.pos_cmd(C.MIDDLEGAME[2]))
        e.send("evalbench 200000")
        line = e.lines_until("info string evalbench", timeout=120)[-1]
    finally:
        e.close()
    with capsys.disabled():
        print("\n" + line)
