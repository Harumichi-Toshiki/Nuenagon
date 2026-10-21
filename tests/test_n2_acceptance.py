# 変更禁止（PROTECTED）。Nuenagon N2（特徴量 RayPair_v0 / v1 の全計算）の受け入れテスト。
#
# 特徴量の定義は設計書 §F2（凍結区画）。正解は付録Bの正解データ（tests/golden/raypair_golden.json、Shikibu と同じもの）と、
# 参照実装 reference/raypair_ref.py で計算したランダムな300局面（tests/golden/n2_golden.json）。
#
# N1 の命令に加えて、次の命令に答えること：
#
#   features <v> <p>        → "features <インデックスを昇順・重複込みで空白区切り>"
#                             v：0 = RayPair_v0、1 = RayPair_v1。p：0 = 先手視点、1 = 後手視点（手番とは無関係の、絶対の視点）。
#                             今の局面（position で作ったもの。moves は差分の処理で進めたもの）から、§F2 の定義どおりに全計算する。
#   linefeatures <v> <p> <L> → "linefeatures <昇順・重複込み>"：視点 p の座標で見たライン L だけの B1 特徴（§F2.1 B1）。
#                             v = 1 なら領域付き（§F2.2）。ライン上の駒が1つ以下なら "linefeatures" だけ。
#   linesof <sq>            → "linesof <L0> <L1> <L2> <L3>"：視点座標のマス sq（= y*9+x）を通る4本のラインの番号（axis 0,1,2,3 の順）。
#
# ラインの番号（視点座標。N3 の差分更新でこの番号を使う）：
#   0〜8   ：axis 0（縦）。x = 0..8
#   9〜17  ：axis 1（横）。y = 0..8 → 番号 9 + y
#   18〜34 ：axis 2（斜め↘、x − y が一定）。c = x − y = −8..8 → 番号 18 + (c + 8)
#   35〜51 ：axis 3（斜め↙、x + y が一定）。s = x + y = 0..16 → 番号 35 + s
#
# 次の条件も満たすこと：全52本の linefeatures を合わせたもの ＝ features の B1 の部分（多重集合として一致）。
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "reference"))
import raypair_ref as ref  # noqa: E402

GOLD = json.load(open(os.path.join(HERE, "golden", "raypair_golden.json"), encoding="utf-8"))
N2 = json.load(open(os.path.join(HERE, "golden", "n2_golden.json"), encoding="utf-8"))
NF = {0: 15000, 1: 115352}


def b1_range(v):
    return (2344, 2344 + (12544 if v == 0 else 9 * 12544))


def line_of(sq):
    y, x = divmod(sq, 9)
    return [x, 9 + y, 18 + (x - y + 8), 35 + (x + y)]


def feats(eng, v, p):
    r = eng.ask(f"features {v} {p}", "features").split()[1:]
    return [int(t) for t in r]


def flip_sfen(s):
    b, stm, h = ref.parse_sfen(s)
    rows = []
    for y in range(9):
        r, e = "", 0
        for x in range(9):
            q = b[8 - y][8 - x]
            if q is None:
                e += 1
                continue
            if e:
                r += str(e)
                e = 0
            c, pt = q
            ch = pt[-1].lower() if c == 0 else pt[-1].upper()
            r += ("+" if pt.startswith("+") else "") + ch
        if e:
            r += str(e)
        rows.append(r)
    hs = ""
    for c in (1, 0):
        for i in range(6, -1, -1):
            n = h[c][i]
            if n:
                ch = ref.HAND_ORDER[i]
                ch = ch.upper() if c == 1 else ch.lower()
                hs += (str(n) if n > 1 else "") + ch
    return "/".join(rows) + (" w " if stm == 0 else " b ") + (hs or "-") + " 1"


# ---------- 付録Bの正解データ ----------

@pytest.mark.parametrize("case", GOLD["cases"], ids=lambda c: f"{c['id']}-v{c['version']}-p{c['perspective']}")
def test_golden_appendix_b(eng, case):
    eng.send(f"position sfen {case['sfen']}")
    got = feats(eng, case["version"], case["perspective"])
    assert got == sorted(got), "昇順に並んでいない"
    assert len(got) == case["count"], f"総数が違う: {len(got)} / 期待 {case['count']}"
    assert sum(got) == case["sum"], "総和が違う"
    assert ref.digest(got) == case["sha16"], "ハッシュが違う"
    assert got == case["indices"], "全リストが違う"


# ---------- ランダムな300局面（差分の処理で進めた局面） ----------

def test_random_positions(eng):
    for i, x in enumerate(N2["positions"]):
        eng.send("position startpos moves " + " ".join(x["moves"]))
        for v in (0, 1):
            for p in (0, 1):
                got = feats(eng, v, p)
                exp = x["f"][f"v{v}p{p}"]
                if got != exp:
                    extra = sorted(set(got) - set(exp))[:10]
                    missing = sorted(set(exp) - set(got))[:10]
                    pytest.fail(f"#{i} v{v} p{p} 不一致: 余分 {extra} 不足 {missing}（sfen {x['sfen']}）")
                assert len(got) <= 208 and all(0 <= t < NF[v] for t in got)


# ---------- 視点の対称性 ----------

def test_perspective_symmetry(eng):
    for x in N2["positions"][:100]:
        s = x["sfen"] + " 1"
        fs = flip_sfen(s)
        for v in (0, 1):
            eng.send(f"position sfen {s}")
            a0, a1 = feats(eng, v, 0), feats(eng, v, 1)
            eng.send(f"position sfen {fs}")
            b0, b1 = feats(eng, v, 0), feats(eng, v, 1)
            assert a0 == b1 and a1 == b0, f"視点の対称性が崩れている: {s} v{v}"


# ---------- ライン単位の取り出し（N3 の差分更新の土台） ----------

def test_linesof(eng):
    for sq in range(81):
        got = [int(t) for t in eng.ask(f"linesof {sq}", "linesof").split()[1:]]
        assert got == line_of(sq), f"マス {sq} のライン番号が違う: {got} / 期待 {line_of(sq)}"


def test_line_decomposition(eng):
    for i, x in enumerate(N2["positions"][:60]):
        eng.send("position startpos moves " + " ".join(x["moves"]))
        for v in (0, 1):
            lo, hi = b1_range(v)
            for p in (0, 1):
                full = [t for t in feats(eng, v, p) if lo <= t < hi]
                parts = []
                for L in range(52):
                    r = [int(t) for t in eng.ask(f"linefeatures {v} {p} {L}", "linefeatures").split()[1:]]
                    assert r == sorted(r)
                    assert all(lo <= t < hi for t in r), f"ライン {L} に B1 以外の特徴が混ざっている"
                    parts += r
                assert sorted(parts) == full, f"#{i} v{v} p{p}：全ラインの和が B1 と一致しない"
