#!/usr/bin/env python3
"""N8.1 の探索統計。12 局面（tests/n5_cases.py の SPEED_POSITIONS）を go nodes N で探索し、
bestmove の後に searchstats を送って、統計の一覧と平均を出す。

  python3 tools/n8_stats.py [--nodes 1000000] [--opts SearchCorrHist=false,...] [--exe build/nuenagon]
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))
import n5_cases as C  # noqa: E402
import test_n8_acceptance as T  # noqa: E402

KEYS = ["nodes", "qratio", "fh", "fh1", "fh1rate", "tt_hit", "tt_cut", "lmr", "lmr_re", "nmp", "nmp_cut", "ebf"]


def parse(line):
    return {k: float(v) for k, v in re.findall(r"(\w+)=([-\d.]+)", line)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--nodes", type=int, default=1000000)
    p.add_argument("--opts", default="")
    p.add_argument("--exe", default=os.path.join(ROOT, "build", "nuenagon"))
    a = p.parse_args()
    opts = [tuple(kv.split("=", 1)) for kv in a.opts.split(",") if kv]
    e = T.start(os.path.abspath(a.exe), [("Deterministic", "true")] + opts)
    rows = []
    try:
        for pos in C.SPEED_POSITIONS:
            e.send("usinewgame")
            e.send(C.pos_cmd(pos))
            e.send(f"go nodes {a.nodes}")
            lines = e.lines_until("bestmove", timeout=300)
            depths = [int(x.split()[2]) for x in lines if x.startswith("info depth ")]
            e.send("searchstats")
            st = e.lines_until("searchstats", timeout=10)[-1]
            d = parse(st)
            d["depth"] = depths[-1] if depths else 0
            rows.append(d)
    finally:
        e.close()
    keys = ["depth"] + KEYS
    print("局面別：" + " ".join(keys))
    for d in rows:
        print("  " + " ".join(f"{d.get(k, 0):.3f}" if k not in ("depth", "nodes") else f"{int(d.get(k, 0))}" for k in keys))
    avg = {k: sum(d.get(k, 0) for d in rows) / len(rows) for k in keys}
    print("平均：depth=%.2f fh1rate=%.3f（fh1=%d/fh=%d） tt_hit=%.3f tt_cut=%.3f qratio=%.3f lmr_re=%.3f nmp_cut=%.3f ebf=%.3f"
          % (avg["depth"], avg["fh1rate"], sum(d["fh1"] for d in rows), sum(d["fh"] for d in rows),
             avg["tt_hit"], avg["tt_cut"], avg["qratio"], avg["lmr_re"], avg["nmp_cut"], avg["ebf"]))
    print("KEYS", " ".join(keys))


if __name__ == "__main__":
    main()
