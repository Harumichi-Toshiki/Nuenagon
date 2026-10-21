# 変更禁止（PROTECTED）。Nuenagon N1（盤面・指し手生成・千日手・詰み探索）の受け入れテスト。
#
# エンジンは標準入力から1行ずつ命令を読み、標準出力に1行ずつ応答する。N1 で必要な命令：
#
#   usi                     → "id name Nuenagon"、"id author Harumichi-Toshiki"、"usiok"
#   isready                 → "readyok"（N1 では評価関数の読み込みは不要）
#   position startpos [moves …] ／ position sfen <盤面> <手番> <持ち駒> <手数> [moves …]
#                           → 応答なし。moves の各手は、探索で使うのと同じ「局面を1手進める処理」（差分更新）で適用し、
#                             千日手の判定のために、局面の履歴（ハッシュ値と王手の有無）を残す。
#   sfen                    → "sfen <盤面> <手番> <持ち駒>"（手数は付けない。持ち駒の並び順は自由。持ち駒なしは "-"）
#   moves                   → "moves <合法手を USI 表記で空白区切り>"（順序は自由。合法手がなければ "moves" だけ）
#   perft <深さ>            → "perft <末端の局面数>"
#   key                     → "key <ハッシュ値を16桁の小文字16進数で>"
#   rep                     → "rep <none|draw|win|lose|superior|inferior>"
#                             今の局面の千日手の状態。判定の決まりは cshogi の Board.is_draw()（手数の制限なし）と同じ：
#                             手番が同じで盤面と両者の持ち駒が同じ局面が、position の履歴の中に1回でもあれば draw。
#                             そのうち、その間の相手の手がすべて王手なら win（手番側の勝ち）、手番側の手がすべて王手なら lose。
#                             盤面が同じで、手番側の持ち駒が以前の局面以上（全駒種で枚数が以上、かつ1種類以上で多い）なら superior、
#                             以下なら inferior。
#   mate <最大ノード数>     → "mate <USI の手>"（手番側が詰ませられる初手）または "mate none"（見つからない）
#   quit                    → 終了
#   知らない命令            → 落ちずに無視する（"info string …" を出してもよい）
#
# 必須の条件：
#   - 盤面・指し手生成・詰み探索は dainagon_source/ のコードを元にしてよい（設計書 §E1）。ただし dainagon_source/ 自体は変更しない。
#   - ONNX Runtime など、評価関数（DL）用の依存を持たない。
import json
import os
import re

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
G = json.load(open(os.path.join(HERE, "golden", "n1_golden.json"), encoding="utf-8"))
HAND_ORDER = "RBGSNLP"


def parse_hand(h):
    """持ち駒の文字列を {駒: 枚数}（大文字＝先手、小文字＝後手）にする。"""
    if h == "-":
        return {}
    out, n = {}, 0
    for c in h:
        if c.isdigit():
            n = n * 10 + int(c)
        else:
            out[c] = out.get(c, 0) + (n or 1)
            n = 0
    return out


def norm_sfen3(s):
    b, t, h = s.split()[:3]
    return b, t, parse_hand(h)


def pos_cmd(x):
    if "sfen" in x and x["sfen"] != "startpos":
        s = x["sfen"]
        if len(s.split()) == 3:
            s += " 1"
        base = f"position sfen {s}"
    else:
        base = "position startpos"
    mv = x.get("moves") or []
    return base + (" moves " + " ".join(mv) if mv else "")


# ---------- USI の最低限 ----------

def test_usi_handshake(eng):
    eng.send("usi")
    assert eng.expect("id name").split(None, 2)[2].startswith("Nuenagon")
    eng.expect("usiok")
    assert eng.ask("isready", "readyok") == "readyok"
    eng.send("this_is_not_a_command 1 2 3")
    assert eng.ask("isready", "readyok") == "readyok"


# ---------- 指し手生成（perft） ----------

@pytest.mark.parametrize("depth", [1, 2, 3, 4, 5])
def test_perft_startpos(eng, depth):
    eng.send("position startpos")
    assert eng.ask(f"perft {depth}", "perft", timeout=300) == f"perft {G['perft_startpos'][depth - 1]}"


@pytest.mark.parametrize("case", G["perft_special"], ids=lambda c: c["name"])
def test_perft_special(eng, case):
    eng.send(pos_cmd({"sfen": case["sfen"]}))
    assert eng.ask(f"perft {case['depth']}", "perft", timeout=300) == f"perft {case['nodes']}"


def test_random_positions(eng):
    """ランダムに指し進めた400局面で、局面の復元・合法手の一覧・perft が cshogi と一致する。"""
    for i, x in enumerate(G["random_positions"]):
        eng.send(pos_cmd({"moves": x["moves"]}))
        got = eng.ask("sfen", "sfen")
        assert norm_sfen3(got[5:]) == norm_sfen3(x["sfen"]), f"#{i} 局面が違う: {got} / 期待 {x['sfen']}"
        mv = sorted(eng.ask("moves", "moves").split()[1:])
        assert mv == x["legal"], f"#{i} 合法手が違う: 余分 {sorted(set(mv) - set(x['legal']))} 不足 {sorted(set(x['legal']) - set(mv))}"
        assert eng.ask(f"perft {x['depth']}", "perft", timeout=120) == f"perft {x['nodes']}", f"#{i} perft が違う"


# ---------- ハッシュ値 ----------

def test_hash_incremental_matches_full(eng):
    """手を進めて作った局面のハッシュ値が、同じ局面を SFEN から直接作った場合と一致し、異なる局面では異なる。"""
    seen = {}
    for i, x in enumerate(G["random_positions"][:200]):
        eng.send(pos_cmd({"moves": x["moves"]}))
        k1 = eng.ask("key", "key")
        eng.send(pos_cmd({"sfen": x["sfen"]}))
        k2 = eng.ask("key", "key")
        assert re.fullmatch(r"key [0-9a-f]{16}", k1), k1
        assert k1 == k2, f"#{i} 差分更新のハッシュ値と全計算のハッシュ値が違う"
        b, t, h = norm_sfen3(x["sfen"])
        ident = (b, t, tuple(sorted(h.items())))
        if ident in seen:
            assert seen[ident] == k1
        else:
            assert k1 not in seen.values(), f"#{i} 異なる局面のハッシュ値が衝突した"
            seen[ident] = k1


# ---------- 千日手 ----------

@pytest.mark.parametrize("kind", ["none", "draw", "win", "lose", "superior", "inferior"])
def test_repetition(eng, kind):
    cases = [x for x in G["repetition"] if x["expect"] == kind]
    assert cases
    for i, x in enumerate(cases):
        eng.send(pos_cmd(x))
        got = eng.ask("rep", "rep")
        assert got == f"rep {kind}", f"#{i} {pos_cmd(x)} → {got}（期待 rep {kind}）"


# ---------- 詰み探索 ----------

def _verify_mate(sfen, mv):
    import cshogi
    b = cshogi.Board(sfen)
    m = b.move_from_usi(mv)
    if not b.is_legal(m):
        return False
    b.push(m)
    if not b.is_check():
        return False
    d = cshogi.DfPn()
    d.set_max_search_node(3_000_000)
    return d.search_andnode(b)


def test_mate_found(eng):
    """11手詰め以下は必ず見つける。13手以上は見つからなくてもよい。どの場合も、間違った詰み手を返してはいけない。"""
    wrong, missed_short = [], []
    for x in G["mate"]:
        eng.send(pos_cmd({"sfen": x["sfen"]}))
        got = eng.ask("mate 1000000", "mate", timeout=120).split()[1]
        if got == "none":
            if x["length"] <= 11:
                missed_short.append((x["length"], x["sfen"]))
        elif not _verify_mate(x["sfen"], got):
            wrong.append((x["length"], x["sfen"], got))
    assert not wrong, f"間違った詰み手を返した: {wrong}"
    assert not missed_short, f"11手詰め以下を見つけられなかった: {missed_short}"


def test_no_false_mate(eng):
    for x in G["nomate"]:
        eng.send(pos_cmd({"sfen": x["sfen"]}))
        got = eng.ask("mate 1000000", "mate", timeout=120)
        assert got == "mate none", f"詰みのない局面で詰みを主張した: {x['sfen']} → {got}"
