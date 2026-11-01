# 変更禁止（PROTECTED）。Nuenagon N7（Shikibu M7 の評価関数への対応：やねうら王の古典的な NNUE を任意の構造で読む）の受け入れテスト。
#
# 指南書：プロジェクトの Nuenagon_M7eval_GUIDE.md（設計書 v1.8 §S12 と対になる）。
# 特徴・推論・ファイルの並びの定義は tests/nnue_ref.py・tests/nnue_nnbin.py（Shikibu のリポジトリと同じファイル）。
# tests/golden/nnue_golden.json は、やねうら王（コミット 32165a8）を 12 通りの構造でビルドし、nnue_nnbin.make で作った
# 乱数の重みで取った静的評価値（FV_SCALE=16）。
#
# ■ 読み込み（EvalFile の先頭4バイトが 0x7AF32F16 なら nn.bin。N5H と同じ）
#   対応する構造：<特徴>_<W>x2-<L1>-<L2>。特徴は halfkp・halfka1・halfka2・halfkahm1・halfkahm2、
#   W は 32 の倍数（32〜8192）、L1・L2 は 8 の倍数（8〜1024）。
#   構造の決め方：構造の文字列は読み飛ばす（中身を当てにしない。N5H と同じ）。特徴変換のハッシュ（= 特徴のハッシュ ^ 2W）から
#   特徴と W を、ネットワークのハッシュとファイルの長さから L1・L2 を決める（nnue_nnbin.hashes・file_size の式）。
#   ファイル全体のハッシュ（= 特徴変換 ^ ネットワーク）も確かめる。どれにも当てはまらなければ読み込みを拒否する。
#   成功：  "info string Nuenagon eval: feature=<名前> network=<構造> W=<W> L1=<L1> L2=<L2> fv_scale=<値>"
#     名前：HalfKP・HalfKA1・HalfKA2・HalfKA_hm1・HalfKA_hm2。構造：例 halfkahm2_512x2-16-32（小文字）。
#   失敗：  "info string eval load error: <理由>" を出し、readyok は出さない（N3・N5H と同じ）。
#
# ■ 命令（N5H と同じ命令を、どの構造でも）
#   eval → "eval raw <int> cp <int>"、acc <p> → "acc <W 個の整数>"、accfull <p>、acccheck <手数> <種>。
#   HalfKA の系統では、相手の玉が動いたときは差分更新、自分の玉が動いたときはその視点の作り直し（hm は左右の反転も）。
#
# ■ 探索：どの構造の評価値でも探索できること。RayPair・N5H の HalfKP（halfkp_256x2-32-32）の動作は変えないこと。
# ■ 速さ：テストでは測らない（報告に evalbench と NPS を書く）。重い構造の高速化（Finny テーブル、L1 の疎な入力など）は任意。
import hashlib
import json
import os
import struct
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cshogi  # noqa: E402
import nnue_nnbin as NB  # noqa: E402
import nnue_ref as N  # noqa: E402
from conftest import Engine  # noqa: E402

GOLD = json.load(open(os.path.join(HERE, "golden", "nnue_golden.json"), encoding="utf-8"))
POS = GOLD["positions"]
NAMES = {"halfkp": "HalfKP", "halfka1": "HalfKA1", "halfka2": "HalfKA2", "halfkahm1": "HalfKA_hm1",
         "halfkahm2": "HalfKA_hm2"}
ITEMS = {f"{it['arch']}_{it['scale']}_{it['seed']}": it for it in GOLD["files"]}


@pytest.fixture(scope="module")
def eng():
    e = Engine()
    yield e
    e.close()


@pytest.fixture(scope="module")
def files(tmp_path_factory):
    d = tmp_path_factory.mktemp("nnue")
    cache = {}

    def get(key):
        if key not in cache:
            it = ITEMS[key]
            p = str(d / f"{key}.bin")
            NB.make(p, it["arch"], it["seed"], it["scale"])
            assert hashlib.sha256(open(p, "rb").read()).hexdigest() == it["sha256"], \
                "nnue_nnbin.make の出力が正解データを作ったときと違う。作業を止めて報告すること"
            cache[key] = (p, N.Net(p, it["arch"]), it)
        return cache[key]
    return get


def load(eng, path, fv=16):
    eng.send(f"setoption name FV_SCALE value {fv}")
    eng.send(f"setoption name EvalFile value {path}")
    eng.send("isready")
    info = eng.expect("info string Nuenagon eval:", timeout=300)
    eng.expect("readyok", timeout=300)
    return info


def ints(line):
    return [int(t) for t in line.split()[1:]]


def eval_line(eng):
    r = eng.ask("eval", "eval").split()
    assert r[1] == "raw" and r[3] == "cp", f"eval の形式が違う: {r}"
    return int(r[2]), int(r[4])


@pytest.mark.parametrize("key", list(ITEMS))
def test_eval_matches_yaneuraou(eng, files, key):
    path, net, it = files(key)
    info = load(eng, path)
    feat, W, L1, L2 = N.parse_arch(it["arch"])
    for k in (f"feature={NAMES[feat]}", f"network={it['arch']}", f"W={W}", f"L1={L1}", f"L2={L2}", "fv_scale=16"):
        assert k in info.split(), f"{key}: 読み込みの info に {k} が無い: {info}"
    for i, (s, want) in enumerate(zip(POS, it["cp"])):
        eng.send(f"position sfen {s}")
        raw, cp = eval_line(eng)
        b = cshogi.Board(s)
        acc, ref_raw, ref_cp = net.forward(b, 16)
        assert ref_cp == want, "参照実装と正解データが違う（テストの前提が崩れている）。作業を止めて報告すること"
        assert (raw, cp) == (ref_raw, want), f"{key}: 評価値が違う（raw {raw} cp {cp} / 期待 raw {ref_raw} cp {want}）: {s}"
        if i % 16 == 0:
            for p in (0, 1):
                a = ints(eng.ask(f"acc {p}", "acc"))
                assert a == [int(x) for x in acc[p]], f"{key}: 視点{p}の蓄積値が違う: {s}"
                assert ints(eng.ask(f"accfull {p}", "accfull")) == a, f"{key}: 視点{p}の全計算が差分の値と違う: {s}"


@pytest.mark.parametrize("key", ["halfkahm2_64x2-16-32_normal_91", "halfka1_32x2-8-8_wide_194",
                                 "halfkahm1_256x2-32-32_wide_32", "halfkp_32x2-8-8_normal_93"])
def test_acccheck(eng, files, key):
    load(eng, files(key)[0])
    for seed in (1, 2):
        eng.send("position startpos")
        r = eng.ask(f"acccheck 20000 {seed}", "acccheck", timeout=600)
        assert r.startswith("acccheck ok"), f"{key}: 差分更新が全計算と合わない: {r}"
    eng.send("position sfen 4k4/9/9/9/9/9/9/9/4K4 b 2r2b4g4s4n4l18p 1")  # 駒箱の駒・玉だけの局面からも
    r = eng.ask("acccheck 3000 5", "acccheck", timeout=600)
    assert r.startswith("acccheck ok"), r


def test_moves_after_position(eng, files):
    """position … moves … で差分更新しながら進めた局面の評価値が、同じ局面を SFEN で与えたときと同じ。"""
    path, net, it = files("halfkahm2_64x2-16-32_wide_191")
    load(eng, path)
    import random
    rng = random.Random(77)
    for g in range(6):
        b = cshogi.Board()
        moves = []
        for _ in range(rng.randrange(20, 120)):
            ms = list(b.legal_moves)
            if not ms:
                break
            m = rng.choice(ms)
            moves.append(cshogi.move_to_usi(m))
            b.push(m)
        eng.send("position startpos moves " + " ".join(moves))
        got = eval_line(eng)
        assert got[1] == net.forward(b, 16)[2], f"指し進めた局面の評価値が違う: {b.sfen()}"


def test_reject_corrupted(eng, files, tmp_path):
    path = files("halfkahm2_64x2-16-32_normal_91")[0]
    good = open(path, "rb").read()
    n = struct.unpack_from("<I", good, 8)[0]
    ft_at = 12 + n

    def patched(off, val):
        b = bytearray(good)
        struct.pack_into("<I", b, off, val)
        return bytes(b)
    bad = {"file_hash": patched(4, 0x12345678), "ft_hash": patched(ft_at, 0x12345678), "short": good[:-1],
           "long": good + b"\x00" * 32}
    for name, data in bad.items():
        p = tmp_path / f"bad_{name}.bin"
        p.write_bytes(data)
        eng.send(f"setoption name EvalFile value {p}")
        eng.send("isready")
        while True:
            line = eng.q.get(timeout=300)
            assert line is not None, "エンジンが終了した"
            if line.startswith("info string eval load error"):
                break
            assert line != "readyok", f"壊れた nn.bin（{name}）を読み込んでしまった"
    assert "feature=HalfKA_hm2" in load(eng, path)


def test_string_is_ignored(eng, files, tmp_path):
    """構造の文字列が空・でたらめでも、ハッシュと長さで構造を決めて読める。"""
    path, net, it = files("halfkp_96x2-16-24_normal_21")
    good = open(path, "rb").read()
    n = struct.unpack_from("<I", good, 8)[0]
    for alt in (b"", b"Features=Something[1->2x2],Network=whatever"):
        p = tmp_path / "alt.bin"
        p.write_bytes(good[:8] + struct.pack("<I", len(alt)) + alt + good[12 + n:])
        info = load(eng, str(p))
        assert "network=halfkp_96x2-16-24" in info.split(), info
        for s, want in list(zip(POS, it["cp"]))[::25]:
            eng.send(f"position sfen {s}")
            assert eval_line(eng)[1] == want


@pytest.mark.parametrize("key", ["halfkahm2_512x2-16-32_normal_71", "halfka2_256x2-32-32_normal_41"])
def test_search(eng, files, key):
    load(eng, files(key)[0])
    for pos in ("position startpos", "position sfen 4k4/9/4g4/9/9/9/9/4R4/4K4 b - 1"):
        eng.send(pos)
        eng.send("go nodes 30000")
        line = eng.expect("bestmove", timeout=300)
        mv = line.split()[1]
        b = cshogi.Board() if pos == "position startpos" else cshogi.Board(pos.split("sfen ")[1])
        assert mv in [cshogi.move_to_usi(m) for m in b.legal_moves], f"{key}: 合法でない bestmove: {line}"
