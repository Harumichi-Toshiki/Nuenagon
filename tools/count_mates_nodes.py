#!/usr/bin/env python3
"""ノード固定の詰将棋の数：tests/golden/n1_golden.json の長い詰将棋（既定は 9 手以上）を、go nodes N で解かせて数える。

  python3 tools/count_mates_nodes.py --exe build/nuenagon --opts "SearchLMR2=true" --nodes 300000 [--min-length 9]

時間で止めると数が ±2 ぶれたので、節点数で止める。最後の info の点数が正の詰み（mate N, N > 0）なら解けたとする。
--common-opts で両方と同じ共通のオプション（EvalFile など）を渡せる。
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from usi_util import Engine, score_of  # noqa: E402

ROOT = os.path.dirname(HERE)
BASE_OPTS = [("Threads", 1), ("USI_Hash", 64), ("NetworkDelay", 0), ("USI_Ponder", "false"),
             ("MultiPV", 1), ("Deterministic", "true")]


def parse_opts(s):
    return [tuple(kv.split("=", 1)) for kv in s.split(",") if kv] if s else []


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--exe", required=True)
    p.add_argument("--opts", default="")
    p.add_argument("--common-opts", default="")
    p.add_argument("--nodes", type=int, default=300000)
    p.add_argument("--min-length", type=int, default=9)
    p.add_argument("--golden", default=os.path.join(ROOT, "tests", "golden", "n1_golden.json"))
    p.add_argument("--verbose", action="store_true")
    a = p.parse_args()
    data = json.load(open(a.golden, encoding="utf-8"))
    cases = [x for x in data["mate"] if x["length"] >= a.min_length]
    e = Engine(a.exe, BASE_OPTS + parse_opts(a.common_opts) + parse_opts(a.opts))
    solved = 0
    try:
        for x in cases:
            e.send("usinewgame")
            infos, _ = e.go(f"position sfen {x['sfen']}", f"go nodes {a.nodes}")
            sc = score_of(infos)
            ok = sc is not None and sc[0] == "mate" and sc[1] > 0
            solved += ok
            if a.verbose:
                print(f"  {x['length']:>2} 手 {'解けた' if ok else '---'}  {sc}", flush=True)
    finally:
        e.close()
    print(f"長い詰将棋（{a.min_length} 手以上、{len(cases)} 問、{a.nodes} ノード）：{solved} 問解けた")
    return 0


if __name__ == "__main__":
    sys.exit(main())
