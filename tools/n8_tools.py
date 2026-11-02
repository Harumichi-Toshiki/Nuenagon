#!/usr/bin/env python3
"""N8 の測定の道具（テストとは別。保護ファイルは読むだけで変えない）。

  det    : 全部の N8 の手法を切った版が、N7（基準の実行ファイル）と決定的モードで同じ探索をするかを確かめる
  depth  : 12 局面を go movetime 1000 で探索し、bestmove の直前の深さの平均を出す（テストの深さと同じ条件）
  match  : 候補 対 N7 の対局（テストの強さと同じ開始局面・同じ手の時間）

例:
  python3 tools/n8_tools.py det   --cand build/nuenagon --ref <N7> [--opts SearchSingular=true,...]
  python3 tools/n8_tools.py depth --cand build/nuenagon --ref <N7> [--opts ...]
  python3 tools/n8_tools.py match --cand build/nuenagon --ref <N7> --games 100 [--opts ...] [--out result.json]
"""
import argparse
import json
import os
import sys
import threading
import queue
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TESTS = os.path.join(ROOT, "tests")
sys.path.insert(0, TESTS)
import n5_cases as C  # noqa: E402
import test_n8_acceptance as T  # noqa: E402


def parse_opts(s):
    out = []
    if s:
        for kv in s.split(","):
            k, v = kv.split("=", 1)
            out.append((k.strip(), v.strip()))
    return out


def cmd_det(a):
    nodes = a.nodes
    ok = True
    for pos in C.SPEED_POSITIONS:
        res = {}
        for name, exe, opts in (("ref", a.ref, []), ("cand", a.cand, parse_opts(a.opts))):
            e = C.Engine(exe)
            try:
                e.send("usi")
                e.lines_until("usiok")
                for k, v in [("EvalFile", T.EVAL), ("Threads", 1), ("USI_Hash", 64), ("NetworkDelay", 0),
                             ("USI_Ponder", "false"), ("MultiPV", 1), ("Deterministic", "true"),
                             ("DebugCheckAccumulator", "false")] + opts:
                    e.send(f"setoption name {k} value {v}")
                e.send("isready")
                e.lines_until("readyok", timeout=300)
                r, _ = C.run_case(e, pos, 1, nodes)
                res[name] = r
            finally:
                e.close()
        same = res["ref"] == res["cand"]
        ok &= same
        print(("SAME " if same else "DIFF ") + pos[:60], res["cand"]["bestmove"] if same else (res["ref"]["bestmove"], res["cand"]["bestmove"]))
    print("ALL SAME" if ok else "DIFFERENT")
    return 0 if ok else 1


def run_depth(exe, opts):
    e = T.start(exe, opts)
    ds = []
    try:
        for p in C.SPEED_POSITIONS:
            e.send("usinewgame")
            infos, _ = T.search(e, C.pos_cmd(p), "go movetime 1000", timeout=30)
            ds.append(T.last_depth(infos))
    finally:
        e.close()
    return sum(ds) / len(ds), ds


def cmd_depth(a):
    opts = parse_opts(a.opts)
    mr, dr = run_depth(a.ref, [])
    mn, dn = run_depth(a.cand, opts)
    print(f"深さ（1 手 1 秒）：基準 {mr:.2f} {dr}")
    print(f"              候補 {mn:.2f} {dn}  差 {mn - mr:+.2f}")
    return 0


def cmd_match(a):
    opts = parse_opts(a.opts)
    ops = T.openings(a.games // 2)
    jobs = queue.Queue()
    for op in ops:
        jobs.put((op, True))
        jobs.put((op, False))
    score = {"w": 0, "l": 0, "d": 0}
    lock = threading.Lock()
    errors = []
    results = []

    def worker():
        cand, ref = T.start(a.cand, opts), T.start(a.ref)
        try:
            while True:
                try:
                    op, new_black = jobs.get_nowait()
                except queue.Empty:
                    return
                r = T.play(cand, ref, op, movetime=a.movetime) if new_black else T.play(ref, cand, op, movetime=a.movetime)
                with lock:
                    if r == "draw":
                        score["d"] += 1
                        res = 0.5
                    elif (r == "black") == new_black:
                        score["w"] += 1
                        res = 1.0
                    else:
                        score["l"] += 1
                        res = 0.0
                    results.append({"opening": " ".join(op), "cand_black": new_black, "result": res})
                    n = score["w"] + score["l"] + score["d"]
                    if n % 10 == 0:
                        print(f"  進み {n}/{a.games}  候補 {score['w']}勝 {score['l']}敗 {score['d']}分", flush=True)
        except Exception as ex:  # noqa: BLE001
            errors.append(repr(ex))
        finally:
            cand.close()
            ref.close()

    t0 = time.time()
    ts = [threading.Thread(target=worker) for _ in range(a.workers)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    n = score["w"] + score["l"] + score["d"]
    rate = (score["w"] + 0.5 * score["d"]) / max(1, n)
    print(f"結果：候補 対 基準 {n} 局 {score['w']} 勝 {score['l']} 敗 {score['d']} 分、勝率 {rate:.3f}（{time.time() - t0:.0f} 秒）")
    print(f"  opts={a.opts or '(なし)'} movetime={a.movetime}")
    if errors:
        print("エラー:", errors[:3])
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump({"opts": a.opts, "movetime": a.movetime, "games": n, "w": score["w"], "l": score["l"],
                       "d": score["d"], "rate": rate, "results": results}, f, ensure_ascii=False, indent=1)
    return 1 if errors else 0


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("det", "depth", "match"):
        q = sub.add_parser(name)
        q.add_argument("--cand", required=True)
        q.add_argument("--ref", required=True)
        q.add_argument("--opts", default="")
        if name == "det":
            q.add_argument("--nodes", type=int, default=100000)
        if name == "match":
            q.add_argument("--games", type=int, default=100)
            q.add_argument("--movetime", type=int, default=200)
            q.add_argument("--workers", type=int, default=2)
            q.add_argument("--out", default="")
    a = p.parse_args()
    a.cand = os.path.abspath(a.cand)
    a.ref = os.path.abspath(a.ref)
    return {"det": cmd_det, "depth": cmd_depth, "match": cmd_match}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
