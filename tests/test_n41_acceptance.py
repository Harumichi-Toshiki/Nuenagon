# 変更禁止（PROTECTED）。Nuenagon N4.1（探索の時間の使い方と、木の膨らみの修正）の受け入れテスト。
#
# N4 の強さのテストは、持ち駒の多い中盤で探索が浅くなり、ときどき負けていた。原因は次の3つ。
#   1. ルートの詰み探索（df-pn）がノード数でしか止まらず、思考時間の大半を使っていた。
#   2. 王手をかける手も延長していたため、持ち駒の多い局面で木が膨らんでいた。
#   3. 静止探索が損な取りも含めて駒を取る手を全部読み、置換表も使っていなかった。
# このテストは、その局面（N4 の強さのテストで実際に負けた対局から取った）で、時間内に十分深く読めることを確かめる。
#
# ■ 詰みの手数（USI の決まり）
#   "score mate <n>" の n は、詰みまでの手数（ply）。勝ちは正の奇数、負け（詰まされる）は負の偶数になる。
#   （最短の詰みでなくてもよい。df-pn は最短とは限らないため。）
#   （チェスの UCI のような「何手目の自分の手で詰むか」ではない。）
#
# ■ 中盤の深さ
#   下の局面で、Threads=1・go movetime 500 のとき、最後の info depth が MIN_DEPTH 以上、bestmove が 0.8 秒以内。
#
# ■ 短い思考時間
#   go movetime 100 で、bestmove が 0.35 秒以内に返り、その前に info depth が少なくとも1行出る。
#   （ルートの詰み探索を含め、すべての探索が締め切りを守ること。）
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_n4_acceptance import setup, search  # noqa: E402

N1 = json.load(open(os.path.join(HERE, "golden", "n1_golden.json"), encoding="utf-8"))

MIDDLEGAME = [
    "ln3+B2l/2s2k3/rpp2pnsp/6pp1/p8/P5P2/1PP2KNPP/6S+bL/LNSGG4 w 2GPr4p 44",
    "ln6l/2s6/r4knsp/1pp2ppp1/P8/6P2/1PPG1KNPP/7+bL/LNSGGS3 w Grb6p 56",
    "ln6l/2s6/r5ns1/5k1pp/PG3pP2/9/1PPG1KNPP/7+bL/LNSGGS3 w 2Prb7p 68",
    "ln6+N/2s6/r6s1/9/PG4k2/4G1P1p/1PP2K1+pP/7+bL/LNSGGS3 w NLPrb10p 82",
]
MIN_DEPTH = 4


def test_mate_score_is_in_plies(eng):
    setup(eng)
    n = 0
    for x in N1["mate"]:
        if x["length"] not in (1, 3):
            continue
        infos, _, _ = search(eng, f"position sfen {x['sfen']}", "go movetime 2000", timeout=30)
        kind, v = infos[-1]["score"]
        v = int(v)
        assert kind == "mate" and v > 0 and v % 2 == 1, \
            f"{x['length']}手詰めの局面で score mate {v}（手数で、正の奇数で出す）: {x['sfen']}"
        n += 1
    assert n >= 5


def test_mated_score_is_in_plies(eng):
    import cshogi
    setup(eng)
    n = 0
    for x in N1["mate"]:
        if x["length"] != 3:
            continue
        b = cshogi.Board(x["sfen"])
        b.push_usi(x["pv"][0])
        infos, _, _ = search(eng, f"position sfen {b.sfen()}", "go movetime 2000", timeout=30)
        kind, v = infos[-1]["score"]
        assert kind == "mate" and int(v) == -2, f"2手後に詰まされる局面で score {kind} {v}（mate -2 のはず）: {b.sfen()}"
        n += 1
    assert n >= 5


def test_middlegame_depth(eng):
    setup(eng)
    for sfen in MIDDLEGAME:
        eng.send("usinewgame")
        infos, best, el = search(eng, f"position sfen {sfen}", "go movetime 500", timeout=30)
        assert infos, f"info depth が出ていない: {sfen}"
        d = int(infos[-1]["depth"])
        assert d >= MIN_DEPTH, f"0.5秒で深さ {d} までしか読めていない（{MIN_DEPTH} 以上が必要）: {sfen}"
        assert el <= 0.8, f"movetime 500 で {el:.2f} 秒かかった: {sfen}"


def test_short_movetime(eng):
    setup(eng)
    for sfen in MIDDLEGAME:
        eng.send("usinewgame")
        infos, best, el = search(eng, f"position sfen {sfen}", "go movetime 100", timeout=30)
        assert el <= 0.35, f"movetime 100 で {el:.2f} 秒かかった（締め切りを守っていない探索がある）: {sfen}"
        assert infos, f"movetime 100 で info depth が1行も出ていない: {sfen}"
