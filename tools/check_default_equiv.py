#!/usr/bin/env python3
"""既定の設定で、新版の探索が基準（別の版）と完全に同じかを確かめる。

  python3 tools/check_default_equiv.py --ref <基準の実行ファイル> --new build/nuenagon [--nodes 300000]

12 局面（tests/n5_cases.py の SPEED_POSITIONS。3 番目は 2b2c を除く）を、決定的モード・Threads=1・go nodes N で読ませ、
info 行（時間と速さの項目を除く）と bestmove が全部一致するかを比べる。
--new-opts で新版だけに足すオプション（例：既定でないものを入れたら、一致しないはず）を渡せる。
評価関数は既定（tests/golden/material_v0.shk）。別の評価関数は --eval で指定する。
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tests"))
from usi_util import Engine, normalize_info  # noqa: E402
import n5_cases as C  # noqa: E402

ROOT = os.path.dirname(HERE)


def parse_opts(s):
    return [tuple(kv.split("=", 1)) for kv in s.split(",") if kv] if s else []


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--ref", required=True)
    p.add_argument("--new", required=True)
    p.add_argument("--new-opts", default="")
    p.add_argument("--eval", default=os.path.join(ROOT, "tests", "golden", "material_v0.shk"))
    p.add_argument("--nodes", type=int, default=300000)
    a = p.parse_args()
    positions = [q.replace(" 2b2c", "") for q in C.SPEED_POSITIONS]
    base = [("EvalFile", a.eval), ("Threads", 1), ("USI_Hash", 64), ("NetworkDelay", 0), ("USI_Ponder", "false"),
            ("MultiPV", 1), ("Deterministic", "true")]
    same = 0
    for i, pos in enumerate(positions):
        pos_cmd = "position " + pos
        outs = []
        for exe, extra in ((a.ref, []), (a.new, parse_opts(a.new_opts))):
            e = Engine(exe, base + extra)
            try:
                infos, bm = e.go(pos_cmd, f"go nodes {a.nodes}")
            finally:
                e.close()
            outs.append(([normalize_info(x) for x in infos], bm))
        ok = outs[0] == outs[1]
        same += ok
        print(f"{i:>2} {'SAME' if ok else 'DIFF'}  基準 {outs[0][1].split()[1]}  新 {outs[1][1].split()[1]}", flush=True)
    print(f"完全に一致：{same}/{len(positions)} 局面")
    return 0 if same == len(positions) else 1


if __name__ == "__main__":
    sys.exit(main())
