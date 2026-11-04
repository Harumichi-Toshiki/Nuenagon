#!/usr/bin/env python3
"""ノード固定の対局：2 つの USI エンジン（A と B）を、それぞれのノード数（go nodes N）で対局させる。

  python3 tools/match_nodes.py --a build/nuenagon --a-opts "SearchLMR2=true" --a-nodes 100000 \\
                               --b build/nuenagon --b-opts "" --b-nodes 100000 \\
                               --games 200 --workers 3 --out results/a_vs_b.jsonl

- Deterministic=true（ノード固定の対局を、同じ版・同じ設定で再現できるように）。
- 開始局面は、既存の対局の道具（tests/test_n8_acceptance.py の openings）と同じ作り方・同じ乱数の種（55）。
- 先後は入れ替えて、各開始局面を 2 局指す（games は偶数）。
- --out に 1 局ごとに追記する（JSON Lines）。同じ --out で再実行すると、終わった局は飛ばして続きから指す。
- 結果の表示：A の勝ち・負け・引き分け、勝率（勝ち + 引き分けの半分）、誤差（±2σ）。
- 途中経過だけ見るなら --summary-only を付ける（指さない）。
- 評価関数などの共通の設定は --common-opts で両方に渡す（例：EvalFile=...,FV_SCALE=13）。
- 対局の終わり方は既存の道具と同じ：投了・詰み、千日手（4 回）、最大手数、大きな評価の差が続いたら勝ち（調停）。
"""
import argparse
import json
import math
import os
import queue
import random
import sys
import threading

import cshogi

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from usi_util import Engine, score_of  # noqa: E402

BASE_OPTS = [("Threads", 1), ("USI_Hash", 64), ("NetworkDelay", 0), ("USI_Ponder", "false"),
             ("MultiPV", 1), ("Deterministic", "true")]  # ノード固定の対局は決定的にする（時間で打ち切る処理を通さない）


def parse_opts(s):
    out = []
    for kv in (s.split(",") if s else []):
        if not kv:
            continue
        k, v = kv.split("=", 1)
        out.append((k.strip(), v.strip()))
    return out


def openings(n, seed=55):
    """既存の対局の道具（tests/test_n8_acceptance.py の openings）と同じ作り方。王手にならない 6 手。"""
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


def play(black, white, opening, black_nodes, white_nodes, max_plies=300, adj=2000, adj_moves=3):
    """1 局。返す値は 'black' / 'white' / 'draw'（先手・後手の勝ち、引き分け）。"""
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
        e, nodes = (black, black_nodes) if stm == cshogi.BLACK else (white, white_nodes)
        pos = "position startpos moves " + " ".join(moves)
        infos, best = e.go(pos, f"go nodes {nodes}")
        u = best.split()[1]
        lose = "white" if stm == cshogi.BLACK else "black"
        win = "black" if stm == cshogi.BLACK else "white"
        if u == "resign":
            return lose
        if u == "win":
            return win if b.is_nyugyoku() else lose
        mv = b.move_from_usi(u)
        if not b.is_legal(mv):
            return lose  # 非合法手を指したら負け（このエンジンでは起きない想定）
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
        sc = score_of(infos)
        if sc:
            last[stm] = sc[1] if sc[0] == "cp" else (100000 if sc[1] > 0 else -100000)
        sb, sw = last[0], last[1]
        if sb is not None and sw is not None and ((sb >= adj and sw <= -adj) or (sb <= -adj and sw >= adj)):
            run += 1
            if run >= 2 * adj_moves:
                return "black" if sb > 0 else "white"
        else:
            run = 0


def load_done(path):
    done = {}
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    r = json.loads(line)
                    done[r["game"]] = r
    return done


def summary(results, label=""):
    n = len(results)
    if n == 0:
        print("まだ 1 局も無い")
        return
    w = sum(1 for r in results if r["a_score"] == 1.0)
    l = sum(1 for r in results if r["a_score"] == 0.0)
    d = n - w - l
    scores = [r["a_score"] for r in results]
    rate = sum(scores) / n
    mean = rate
    var = sum((s - mean) ** 2 for s in scores) / max(1, n - 1)
    se = math.sqrt(var / n)
    print(f"{label}A 対 B：{n} 局  A {w} 勝 {l} 敗 {d} 分  勝率 {rate:.3f}  誤差（±2σ）±{2 * se:.3f}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--a", required=True, help="エンジン A の実行ファイル")
    p.add_argument("--b", required=True, help="エンジン B の実行ファイル")
    p.add_argument("--a-opts", default="", help="A だけに渡すオプション（k=v,k=v）")
    p.add_argument("--b-opts", default="", help="B だけに渡すオプション（k=v,k=v）")
    p.add_argument("--common-opts", default="", help="両方に渡すオプション（例：EvalFile=...,FV_SCALE=13）")
    p.add_argument("--a-nodes", type=int, default=100000)
    p.add_argument("--b-nodes", type=int, default=100000)
    p.add_argument("--games", type=int, default=200)
    p.add_argument("--workers", type=int, default=2)
    p.add_argument("--out", default="", help="1 局ごとに追記する JSON Lines（続きから指すのにも使う）")
    p.add_argument("--max-plies", type=int, default=300)
    p.add_argument("--summary-only", action="store_true")
    a = p.parse_args()

    results = list(load_done(a.out).values())
    if a.summary_only:
        summary(results)
        return 0
    ops = openings(a.games // 2)
    todo = queue.Queue()
    done_ids = {r["game"] for r in results}
    for i, op in enumerate(ops):
        for side in (0, 1):  # 0：A が先手、1：B が先手
            gid = i * 2 + side
            if gid not in done_ids:
                todo.put((gid, i, op, side))
    lock = threading.Lock()
    errors = []
    common = parse_opts(a.common_opts)

    def worker():
        ea = eb = None
        try:
            ea = Engine(a.a, BASE_OPTS + common + parse_opts(a.a_opts))
            eb = Engine(a.b, BASE_OPTS + common + parse_opts(a.b_opts))
            while True:
                try:
                    gid, i, op, side = todo.get_nowait()
                except queue.Empty:
                    return
                if side == 0:
                    res = play(ea, eb, op, a.a_nodes, a.b_nodes, a.max_plies)
                else:
                    res = play(eb, ea, op, a.b_nodes, a.a_nodes, a.max_plies)
                a_black = side == 0
                a_win = (res == "black") == a_black
                score = 0.5 if res == "draw" else (1.0 if a_win else 0.0)
                rec = {"game": gid, "opening": i, "a_black": a_black, "result": res, "a_score": score}
                with lock:
                    results.append(rec)
                    if a.out:
                        with open(a.out, "a", encoding="utf-8") as f:
                            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
                    if len(results) % 10 == 0:
                        summary(results, label=f"  進み {len(results)}/{a.games}：")
        except Exception as ex:  # noqa: BLE001
            errors.append(repr(ex))
        finally:
            for e in (ea, eb):
                if e:
                    e.close()

    ts = [threading.Thread(target=worker) for _ in range(a.workers)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    print("--- 結果 ---")
    summary(results)
    if errors:
        print("エラー:", errors[:3])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
