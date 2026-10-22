# 変更禁止（PROTECTED）。Nuenagon N5.5（探索の改良：指し手の生成と並べ替え、枝刈りと延長、将棋ならではの工夫）の受け入れテスト。
#
# N5.5 では探索の木を自由に変えてよい（設計書 v1.6 §E11）。良し悪しは、N5H（コミット 898cd01）と比べて決める。
# テストが N5H を git から取り出して一時フォルダにビルドし、同じ環境で比べる。
# 評価関数は tests/golden/raypair_v0_trained.shk（持ち主が Shikibu で学習した RayPair_v0。本番に近い条件で比べるため）。
# （改訂：最初の版は material_v0.shk だった。駒の損得しか分からない評価関数では、静的評価に頼る枝刈りの効き目を正しく測れないため変えた。）
#
# ■ 深さ：tests/n5_cases.py の SPEED_POSITIONS（12 局面）を、Threads=1・go movetime 1000 で探索し、
#   bestmove の直前の info の depth の平均が、N5H より 2.0 以上大きい。
#
# ■ 強さ：N5H と 100 局（同じ開始局面で先後を入れ替えた 2 局 × 50 組）、1 手 0.2 秒（go movetime 200）、Threads=1・USI_Hash=64 で
#   指し、勝率（引き分けは 0.5）が 0.60 以上。開始局面は、初手から乱数で 6 手（王手にならない手）進めた局面（種は固定）。
#   千日手は同じ局面が 4 回で引き分け、300 手で引き分け、両者の評価値が同じ側の勝ちを 2000 点以上で 3 手ずつ示したら打ち切り。
#
# ■ 詰み：tests/golden/n1_golden.json の詰将棋のうち 9 手以上のものを、Threads=1・go movetime 3000 で探索し、
#   最後の info が "score mate <正>" になった問題の数が、N5H 以上。
#
# ■ 入り切りできる手法：usi の応答に、次の check のオプション（既定 true）がある。
#     SearchLMRTable, SearchRFP, SearchNullMoveDynamic, SearchLMP, SearchSEEPrune, SearchContHistory, SearchMate1Ply, SearchDropPrune
#   ほかに入り切りできる手法を作ったら、名前を "Search" で始まる check のオプションにする（既定は true でも false でもよい）。
#   "Search" で始まる check のオプションを全部 false にしても、合法手を指し、5 手以下の詰将棋で詰みを見つける。
import json
import os
import queue
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import cshogi  # noqa: E402
import n5_cases as C  # noqa: E402

REF_COMMIT = "898cd01"
MATERIAL = C.MATERIAL
EVAL = os.path.join(HERE, "golden", "raypair_v0_trained.shk")
N1 = C.N1
REQUIRED = ["SearchLMRTable", "SearchRFP", "SearchNullMoveDynamic", "SearchLMP", "SearchSEEPrune",
            "SearchContHistory", "SearchMate1Ply", "SearchDropPrune"]


def exe_in(build_dir):
    for c in ("nuenagon", "Release/nuenagon.exe", "nuenagon.exe"):
        p = os.path.join(build_dir, c)
        if os.path.exists(p):
            return p
    raise FileNotFoundError(f"{build_dir} に nuenagon が無い")


NEW = lambda: exe_in(os.path.join(ROOT, "build"))  # noqa: E731


@pytest.fixture(scope="module")
def ref_exe():
    tmp = tempfile.mkdtemp(prefix="nue_ref55_")
    wt = os.path.join(tmp, "src")
    r = subprocess.run(["git", "-C", ROOT, "worktree", "add", "--detach", wt, REF_COMMIT], capture_output=True, text=True)
    assert r.returncode == 0, f"N5H（{REF_COMMIT}）を取り出せない: {r.stderr}"
    try:
        bd = os.path.join(tmp, "build")
        subprocess.run(["cmake", "-S", wt, "-B", bd, "-DCMAKE_BUILD_TYPE=Release"], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", bd, "-j"], check=True, stdout=subprocess.DEVNULL)
        yield exe_in(bd)
    finally:
        subprocess.run(["git", "-C", ROOT, "worktree", "remove", "--force", wt], capture_output=True)
        shutil.rmtree(tmp, ignore_errors=True)


def start(exe, opts=()):
    e = C.Engine(exe)
    e.send("usi")
    usi = e.lines_until("usiok")
    base = [("EvalFile", EVAL), ("Threads", 1), ("USI_Hash", 64), ("NetworkDelay", 0), ("USI_Ponder", "false"),
            ("MultiPV", 1), ("Deterministic", "false"), ("DebugCheckAccumulator", "false")]
    for k, v in base + list(opts):
        e.send(f"setoption name {k} value {v}")
    e.send("isready")
    e.lines_until("readyok", timeout=300)
    e.usi_lines = usi
    return e


def search(e, pos, go, timeout=60):
    e.send(pos)
    e.send(go)
    lines = e.lines_until("bestmove", timeout=timeout)
    infos = [x for x in lines if x.startswith("info depth ")]
    return infos, lines[-1]


def last_depth(infos):
    return int(infos[-1].split()[2]) if infos else 0


def mate_score(infos):
    if not infos:
        return None
    t = infos[-1].split()
    if "mate" in t:
        return int(t[t.index("mate") + 1])
    return None


# ---------------------------------------------------------------- 深さ

def test_depth_gain(ref_exe):
    res = {}
    for name, exe in (("ref", ref_exe), ("new", NEW())):
        e = start(exe)
        ds = []
        try:
            for p in C.SPEED_POSITIONS:
                e.send("usinewgame")
                infos, _ = search(e, C.pos_cmd(p), "go movetime 1000", timeout=30)
                ds.append(last_depth(infos))
        finally:
            e.close()
        res[name] = ds
    mr, mn = sum(res["ref"]) / len(res["ref"]), sum(res["new"]) / len(res["new"])
    print(f"\n深さ（1 手 1 秒）：N5H 平均 {mr:.2f} {res['ref']} ／ 新 平均 {mn:.2f} {res['new']}")
    assert mn >= mr + 2.0, f"深さの平均が N5H {mr:.2f} → 新 {mn:.2f}（+2.0 以上が必要）"


# ---------------------------------------------------------------- 詰み

def test_long_mates_not_worse(ref_exe):
    cases = [x for x in N1["mate"] if x["length"] >= 9]
    assert len(cases) >= 5
    solved = {}
    for name, exe in (("ref", ref_exe), ("new", NEW())):
        e = start(exe)
        n = 0
        try:
            for x in cases:
                e.send("usinewgame")
                infos, _ = search(e, f"position sfen {x['sfen']}", "go movetime 3000", timeout=30)
                m = mate_score(infos)
                if m is not None and m > 0:
                    n += 1
        finally:
            e.close()
        solved[name] = n
    print(f"\n長い詰将棋（{len(cases)} 問）：N5H {solved['ref']} 問 ／ 新 {solved['new']} 問")
    assert solved["new"] >= solved["ref"], f"長い詰将棋を解けた数が減った（N5H {solved['ref']} → 新 {solved['new']}）"


# ---------------------------------------------------------------- 入り切りできる手法

def search_options(usi_lines):
    out = {}
    for line in usi_lines:
        t = line.split()
        if len(t) >= 7 and t[0] == "option" and t[1] == "name" and t[2].startswith("Search") and t[4] == "check":
            out[t[2]] = t[6]
    return out


def test_required_options():
    e = start(NEW())
    try:
        opts = search_options(e.usi_lines)
    finally:
        e.close()
    missing = [k for k in REQUIRED if k not in opts]
    assert not missing, f"入り切りのオプションが無い: {missing}（あるもの: {sorted(opts)}）"
    off = [k for k in REQUIRED if opts[k] != "true"]
    assert not off, f"既定が true でない: {off}"


def test_all_options_off_still_correct():
    e = start(NEW())
    opts = search_options(e.usi_lines)
    e.close()
    e = start(NEW(), [(k, "false") for k in opts])
    try:
        for x in [c for c in N1["mate"] if c["length"] <= 5][:10]:
            e.send("usinewgame")
            infos, best = search(e, f"position sfen {x['sfen']}", "go movetime 1500", timeout=30)
            m = mate_score(infos)
            assert m is not None and m > 0, f"手法を全部切ると詰みを見つけない: {x['sfen']} / {infos[-1] if infos else '(info なし)'}"
        b = cshogi.Board()
        moves = []
        for _ in range(40):
            infos, best = search(e, "position startpos" + (" moves " + " ".join(moves) if moves else ""), "go movetime 100")
            u = best.split()[1]
            if u in ("resign", "win"):
                break
            assert b.is_legal(b.move_from_usi(u)), f"手法を全部切ると非合法手 {u}: {b.sfen()}"
            b.push_usi(u)
            moves.append(u)
            if b.is_game_over():
                break
    finally:
        e.close()


# ---------------------------------------------------------------- 強さ

def openings(n, seed=55):
    rng = random.Random(seed)
    out = []
    while len(out) < n:
        b = cshogi.Board()
        ms = []
        ok = True
        for _ in range(6):
            legal = [m for m in b.legal_moves]
            rng.shuffle(legal)
            pick = None
            for m in legal:
                b.push(m)
                chk = b.is_check()
                b.pop()
                if not chk:
                    pick = m
                    break
            if pick is None:
                ok = False
                break
            b.push(pick)
            ms.append(cshogi.move_to_usi(pick))
        if ok:
            out.append(ms)
    return out


def key_of(b):
    return " ".join(b.sfen().split()[:3])


def play(black, white, opening, movetime=200, max_plies=300, adj=2000, adj_moves=3):
    """1 局指して 'black' / 'white' / 'draw' を返す。"""
    b = cshogi.Board()
    for u in opening:
        b.push_usi(u)
    moves = list(opening)
    for e in (black, white):
        e.send("usinewgame")
    seen = {key_of(b): 1}
    last = {0: None, 1: None}
    run = 0
    while True:
        stm = b.turn
        e = black if stm == cshogi.BLACK else white
        infos, best = search(e, "position startpos moves " + " ".join(moves), f"go movetime {movetime}", timeout=30)
        u = best.split()[1]
        lose = "white" if stm == cshogi.BLACK else "black"
        win = "black" if stm == cshogi.BLACK else "white"
        if u == "resign":
            return lose
        if u == "win":
            return win if b.is_nyugyoku() else lose
        mv = b.move_from_usi(u)
        assert b.is_legal(mv), f"非合法手 {u}: {b.sfen()}"
        b.push(mv)
        moves.append(u)
        if b.is_game_over():
            return win
        k = key_of(b)
        seen[k] = seen.get(k, 0) + 1
        if seen[k] >= 4:
            return "draw"
        if len(moves) >= max_plies:
            return "draw"
        if infos:
            t = infos[-1].split()
            if "cp" in t:
                last[stm] = int(t[t.index("cp") + 1])
            elif "mate" in t:
                last[stm] = 100000 if int(t[t.index("mate") + 1]) > 0 else -100000
        sb, sw = last[0], last[1]
        if sb is not None and sw is not None and ((sb >= adj and sw <= -adj) or (sb <= -adj and sw >= adj)):
            run += 1
            if run >= 2 * adj_moves:
                return "black" if sb > 0 else "white"
        else:
            run = 0


def test_strength_vs_n5h(ref_exe):
    ops = openings(50)
    jobs = queue.Queue()
    for i, op in enumerate(ops):
        jobs.put((op, True))
        jobs.put((op, False))
    score = {"w": 0, "l": 0, "d": 0}
    lock = threading.Lock()
    errors = []
    workers = max(1, min(2, (os.cpu_count() or 2) // 2))

    def worker():
        new, ref = start(NEW()), start(ref_exe)
        try:
            while True:
                try:
                    op, new_black = jobs.get_nowait()
                except queue.Empty:
                    return
                r = play(new, ref, op) if new_black else play(ref, new, op)
                with lock:
                    if r == "draw":
                        score["d"] += 1
                    elif (r == "black") == new_black:
                        score["w"] += 1
                    else:
                        score["l"] += 1
        except Exception as ex:  # noqa: BLE001
            errors.append(repr(ex))
        finally:
            new.close()
            ref.close()

    ts = [threading.Thread(target=worker) for _ in range(workers)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert not errors, f"対局中のエラー: {errors[:3]}"
    n = score["w"] + score["l"] + score["d"]
    rate = (score["w"] + 0.5 * score["d"]) / n
    print(f"\n強さ：新 対 N5H {n} 局 {score['w']} 勝 {score['l']} 敗 {score['d']} 分、勝率 {rate:.3f}")
    assert n == 100
    assert rate >= 0.60, f"N5H に対する勝率 {rate:.3f}（0.60 以上が必要）: {score}"
