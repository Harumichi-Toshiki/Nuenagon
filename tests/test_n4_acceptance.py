# 変更禁止（PROTECTED）。Nuenagon N4（探索の中核・USI・ponder・時間管理）の受け入れテスト。
#
# 重みは tests/golden/material_v0.shk（駒の損得を学習させた RayPair_v0。Shikibu で学習して書き出したもの）を使う。
# 本物の学習済みモデルではないので、ここで確かめるのは「探索が正しく働くか」であって、将棋の強さそのものではない。
#
# ■ USI（設計書 §E2）
#   usi への応答に、少なくとも次のオプションを含める（"option name <名前> type …"）：
#     Threads, USI_Hash, EvalFile, EvalScale, NetworkDelay, USI_Ponder, MaxPly, DrawValue,
#     CpuAffinity, UsePhysicalCoresOnly, MultiPV, Deterministic, DebugCheckAccumulator
#   go：btime / wtime / byoyomi / binc / winc / movetime / nodes / depth / infinite / ponder / searchmoves に対応する。
#   info：反復の各深さの終わりと bestmove の直前に、
#     "info depth <d> seldepth <sd> [multipv <k>] score <cp <v>|mate <n>> nodes <n> nps <nps> time <ms> [hashfull <h>] pv <手…>"
#     を出す（§E7）。pv は今の局面から合法な手順。詰みの評価は mate で表し、cp には詰みの値を出さない（|cp| < 30000）。
#   bestmove：USI_Ponder が true で読み筋が2手以上なら "bestmove <手> ponder <手>"。
#   ponder：§E2.2 のとおり。
#   重みが読み込まれていないときに go が来たら、落ちずに bestmove を返す（手は何でもよい。"resign" でもよい）。
#
# ■ 探索（§E5.1）
#   MultiPV（オプション。既定1）、searchmoves（go の引数）に対応する（§E5.4 のルートの手ごとの記録の上に作る）。
#   Deterministic（check、既定 false）：true のとき、Threads=1・go nodes N なら、同じ局面で毎回まったく同じ
#   info の depth・score・nodes・pv と bestmove を出す（§E5.9）。
#   DebugCheckAccumulator が true なら、探索中に一定間隔で差分更新と全計算を比べ、不一致なら
#   "info string accumulator mismatch …" を出す（§E3.4）。
#
# ■ 強さ
#   tests/n4_baseline.py の対戦相手（2手読み＋駒の損得）と、1手0.5秒で10局指し、7勝以上・負けなし（最大400手、千日手は引き分け）。
#
# ■ 開発用の命令
#   bench → "bench nodes <n> nps <nps>"（固定の局面群を固定の深さで探索した合計）。
import json
import os
import re
import sys
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from n4_baseline import Baseline  # noqa: E402

SHK = os.path.join(HERE, "golden", "material_v0.shk")
N1 = json.load(open(os.path.join(HERE, "golden", "n1_golden.json"), encoding="utf-8"))
REQUIRED_OPTIONS = ["Threads", "USI_Hash", "EvalFile", "EvalScale", "NetworkDelay", "USI_Ponder", "MaxPly",
                    "DrawValue", "CpuAffinity", "UsePhysicalCoresOnly", "MultiPV", "Deterministic",
                    "DebugCheckAccumulator"]


def setup(eng, **opts):
    eng.send(f"setoption name EvalFile value {SHK}")
    base = {"Threads": 1, "USI_Hash": 64, "MultiPV": 1, "USI_Ponder": "false", "Deterministic": "false",
            "DebugCheckAccumulator": "false", "NetworkDelay": 0}
    base.update(opts)
    for k, v in base.items():
        eng.send(f"setoption name {k} value {v}")
    eng.send("isready")
    eng.expect("readyok", timeout=120)
    eng.send("usinewgame")


def parse_info(line):
    t = line.split()
    d = {}
    i = 1
    while i < len(t):
        k = t[i]
        if k == "pv":
            d["pv"] = t[i + 1:]
            break
        if k == "score":
            d["score"] = (t[i + 1], t[i + 2])
            i += 3
            continue
        if k == "string":
            d["string"] = " ".join(t[i + 1:])
            break
        d[k] = t[i + 1] if i + 1 < len(t) else None
        i += 2
    return d


def search(eng, pos, go, timeout=60):
    """position と go を送り、(info の辞書のリスト, bestmove の行, 経過秒) を返す。"""
    eng.send(pos)
    t = time.time()
    eng.send(go)
    infos = []
    while True:
        line = eng.q.get(timeout=timeout)
        assert line is not None, "エンジンが終了した"
        if line.startswith("info ") and " pv " in line and "string" not in line.split()[1:2]:
            infos.append(parse_info(line))
        if line.startswith("bestmove"):
            return infos, line, time.time() - t


def board_from(pos):
    import cshogi
    t = pos.split()
    if t[1] == "startpos":
        b = cshogi.Board()
        rest = t[2:]
    else:
        b = cshogi.Board(" ".join(t[2:6]))
        rest = t[6:]
    if rest and rest[0] == "moves":
        for m in rest[1:]:
            b.push_usi(m)
    return b


def assert_legal_pv(pos, pv):
    import cshogi
    b = board_from(pos)
    for m in pv:
        mv = b.move_from_usi(m)
        assert b.is_legal(mv), f"読み筋に非合法手 {m}（{pos} / pv {' '.join(pv)}）"
        b.push(mv)


# ---------- USI ----------

def test_usi_options(eng):
    eng.send("usi")
    lines = []
    while True:
        line = eng.q.get(timeout=30)
        lines.append(line)
        if line == "usiok":
            break
    names = [re.match(r"option name (\S+)", l).group(1) for l in lines if l.startswith("option name ")]
    missing = [o for o in REQUIRED_OPTIONS if o not in names]
    assert not missing, f"オプションが足りない: {missing}"


def test_go_without_weights():
    from conftest import Engine
    e = Engine()
    try:
        e.send("usi")
        e.expect("usiok")
        e.send("isready")
        e.expect("readyok", timeout=60)
        e.send("position startpos")
        e.send("go depth 2")
        e.expect("bestmove", timeout=30)
    finally:
        e.close()


def test_info_format_and_legal_pv(eng):
    setup(eng)
    for pos in ["position startpos", "position startpos moves 7g7f 3c3d 8h2b+",
                "position startpos moves " + " ".join(N1["random_positions"][3]["moves"])]:
        infos, best, _ = search(eng, pos, "go depth 5")
        assert infos, "info が出ていない"
        for d in infos:
            for k in ("depth", "seldepth", "score", "nodes", "nps", "time", "pv"):
                assert k in d, f"info に {k} が無い: {d}"
            assert d["score"][0] in ("cp", "mate")
            if d["score"][0] == "cp":
                assert abs(int(d["score"][1])) < 30000, "cp に詰みの値が出ている"
            assert_legal_pv(pos, d["pv"])
        assert [int(d["depth"]) for d in infos][-1] >= 5
        bm = best.split()[1]
        assert bm == infos[-1]["pv"][0], "bestmove が最後の読み筋の初手と違う"
        assert_legal_pv(pos, [bm])


# ---------- 詰み ----------

def _is_mating_move(sfen, mv):
    import cshogi
    b = cshogi.Board(sfen)
    m = b.move_from_usi(mv)
    if not b.is_legal(m):
        return False
    b.push(m)
    d = cshogi.DfPn()
    d.set_max_search_node(2_000_000)
    return b.is_check() and d.search_andnode(b)


def test_finds_mate(eng):
    setup(eng)
    cases = [x for x in N1["mate"] if x["length"] <= 7]
    assert len(cases) >= 20
    for x in cases:
        infos, best, _ = search(eng, f"position sfen {x['sfen']}", "go movetime 3000", timeout=30)
        bm = best.split()[1]
        assert _is_mating_move(x["sfen"], bm), f"{x['length']}手詰めで詰ませる手を指さなかった: {x['sfen']} → {bm}"
        last = infos[-1]["score"]
        assert last[0] == "mate" and int(last[1]) > 0, f"詰みを mate で報告していない: {last}（{x['sfen']}）"


def test_reports_being_mated(eng):
    import cshogi
    setup(eng)
    n = 0
    for x in N1["mate"]:
        if x["length"] != 3:
            continue
        b = cshogi.Board(x["sfen"])
        b.push_usi(x["pv"][0])          # 攻め方が1手指した局面：受け方は2手後に詰む
        infos, best, _ = search(eng, f"position sfen {b.sfen()}", "go movetime 2000", timeout=30)
        last = infos[-1]["score"]
        assert last[0] == "mate" and int(last[1]) < 0, f"詰まされることを報告していない: {last}（{b.sfen()}）"
        n += 1
    assert n >= 5


# ---------- 時間管理・stop・ponder ----------

def test_byoyomi_and_movetime(eng):
    setup(eng)
    pos = "position startpos moves " + " ".join(N1["random_positions"][10]["moves"])
    _, _, sec = search(eng, pos, "go btime 0 wtime 0 byoyomi 1000")
    assert sec < 1.3, f"秒読み1秒で {sec:.2f} 秒かかった"
    _, _, sec = search(eng, pos, "go movetime 800")
    assert 0.3 < sec < 1.1, f"movetime 800 で {sec:.2f} 秒"
    _, _, sec = search(eng, pos, "go btime 20000 wtime 20000 binc 1000 winc 1000")
    assert sec < 5.0, f"持ち時間20秒＋加算1秒で {sec:.2f} 秒も使った"


def test_infinite_and_stop(eng):
    setup(eng)
    eng.send("position startpos")
    eng.send("go infinite")
    time.sleep(1.5)
    assert eng.q.empty() or not any(l and l.startswith("bestmove") for l in list(eng.q.queue)), \
        "go infinite なのに bestmove を出した"
    t = time.time()
    eng.send("stop")
    eng.expect("bestmove", timeout=5)
    assert time.time() - t < 0.5, "stop から bestmove まで時間がかかりすぎ"


def test_ponder(eng):
    setup(eng, USI_Ponder="true")
    pos = "position startpos moves 7g7f 3c3d"
    infos, best, _ = search(eng, "position startpos moves 7g7f", "go depth 6")
    t = best.split()
    assert len(t) == 4 and t[2] == "ponder", f"USI_Ponder=true なのに ponder が無い: {best}"
    assert_legal_pv("position startpos moves 7g7f", [t[1], t[3]])
    # 予想が当たる場合
    eng.send(pos)
    eng.send("go ponder btime 0 wtime 0 byoyomi 1000")
    time.sleep(2.0)
    assert not any(l and l.startswith("bestmove") for l in list(eng.q.queue)), "ponder 中に bestmove を出した"
    t0 = time.time()
    eng.send("ponderhit")
    eng.expect("bestmove", timeout=5)
    assert time.time() - t0 < 1.4, "ponderhit の後、秒読み1秒を超えた"
    # 予想が外れる場合
    eng.send(pos)
    eng.send("go ponder btime 0 wtime 0 byoyomi 1000")
    time.sleep(1.0)
    t0 = time.time()
    eng.send("stop")
    eng.expect("bestmove", timeout=5)
    assert time.time() - t0 < 0.5


# ---------- MultiPV・searchmoves ----------

def test_multipv(eng):
    setup(eng, MultiPV=3)
    pos = "position startpos"
    infos, best, _ = search(eng, pos, "go depth 4")
    last_depth = max(int(d["depth"]) for d in infos)
    final = [d for d in infos if int(d["depth"]) == last_depth and "multipv" in d]
    ks = sorted({int(d["multipv"]) for d in final})
    assert ks == [1, 2, 3], f"MultiPV 3 で multipv {ks}"
    firsts = {d["pv"][0] for d in final}
    assert len(firsts) == 3, "MultiPV の各読み筋の初手が重複している"
    setup(eng, MultiPV=1)


def test_searchmoves(eng):
    setup(eng)
    for allowed in (["9g9f"], ["1g1f", "1i1h"], ["5i4h", "5i6h", "5i5h"]):
        infos, best, _ = search(eng, "position startpos", "go depth 4 searchmoves " + " ".join(allowed))
        assert best.split()[1] in allowed, f"searchmoves {allowed} の外の手 {best}"


# ---------- 決定的モード・スレッド・検査 ----------

def test_deterministic(eng):
    pos = "position startpos moves " + " ".join(N1["random_positions"][20]["moves"])
    runs = []
    for _ in range(2):
        setup(eng, Deterministic="true", Threads=1)
        infos, best, _ = search(eng, pos, "go nodes 30000")
        runs.append(([(d["depth"], d["score"], d["nodes"], tuple(d["pv"])) for d in infos], best))
    assert runs[0] == runs[1], "決定的モードなのに結果が違う"
    setup(eng, Deterministic="false")


def test_threads_scale(eng):
    cpus = os.cpu_count() or 1
    if cpus < 2:
        pytest.skip("論理CPUが1つしかない")
    th = min(4, cpus)
    pos = "position startpos moves " + " ".join(N1["random_positions"][30]["moves"])
    res = {}
    for n in (1, th):
        setup(eng, Threads=n)
        infos, best, _ = search(eng, pos, "go movetime 1500")
        assert_legal_pv(pos, [best.split()[1]])
        res[n] = int(infos[-1]["nodes"])
    assert res[th] >= 1.3 * res[1], f"Threads={th} のノード数 {res[th]} が Threads=1 の {res[1]} の1.3倍未満"
    setup(eng, Threads=1)


def test_debug_check_accumulator(eng):
    setup(eng, DebugCheckAccumulator="true")
    for x in N1["random_positions"][40:46]:
        eng.send("position startpos moves " + " ".join(x["moves"]))
        eng.send("go nodes 20000")
        lines = []
        while True:
            line = eng.q.get(timeout=60)
            lines.append(line)
            if line.startswith("bestmove"):
                break
        assert not any("accumulator mismatch" in l for l in lines), "探索中に差分更新の不一致"
    setup(eng, DebugCheckAccumulator="false")


def test_bench(eng):
    setup(eng)
    r = eng.ask("bench", "bench", timeout=300)
    m = re.match(r"bench nodes (\d+) nps (\d+)", r)
    assert m and int(m.group(1)) > 0, r


# ---------- 強さ：簡単な対戦相手に勝ち越す ----------

def play_game(eng, baseline, engine_black, max_plies=400, movetime=500):
    import cshogi
    b = cshogi.Board()
    moves = []
    for ply in range(max_plies):
        engine_turn = (b.turn == cshogi.BLACK) == engine_black
        if engine_turn:
            eng.send("position startpos" + (" moves " + " ".join(moves) if moves else ""))
            eng.send(f"go movetime {movetime}")
            bm = eng.expect("bestmove", timeout=30).split()[1]
            if bm == "resign":
                return "baseline"
            if bm == "win":
                return "engine"
            mv = b.move_from_usi(bm)
            assert b.is_legal(mv), f"エンジンが非合法手 {bm} を指した（{b.sfen()}）"
        else:
            bm = baseline.choose(b)
            mv = b.move_from_usi(bm)
        b.push(mv)
        moves.append(bm)
        if not list(b.legal_moves):
            return "engine" if engine_turn else "baseline"
        if b.is_draw() == cshogi.REPETITION_DRAW:
            return "draw"
    return "draw"


def test_beats_baseline(eng):
    setup(eng)
    res = {"engine": 0, "baseline": 0, "draw": 0}
    for g in range(10):
        r = play_game(eng, Baseline(seed=1000 + g), engine_black=(g % 2 == 0))
        res[r] += 1
        eng.send("usinewgame")
    assert res["engine"] >= 7 and res["baseline"] == 0, f"簡単な対戦相手に勝ち越せない: {res}"
