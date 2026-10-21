# 変更禁止（PROTECTED）。Nuenagon N3（重みの読み込み・量子化・推論・差分更新）の受け入れテスト。
#
# 重みファイル（.shk）の形式は設計書 §F4、量子化と推論は §E4、差分更新は §E3。
# テストは、設計書から書いた独立の参照実装（tests/m4_ref.py と reference/raypair_ref.py。Shikibu と同じもの）で
# 期待値を計算する。重みは、テストの中で乱数から .shk を作る（境目の値や、蓄積値が大きくなる重みも含む）。
#
# N1・N2 の命令に加えて、次の命令に答えること：
#
#   setoption name EvalFile value <パス>   → 応答なし。次の isready で読み込む。
#   setoption name EvalScale value <整数>  → 応答なし。0 ならファイルの eval_scale を使う（§E2.3）。
#   isready
#     読み込みに成功したら、次の2行を出す（§E2.5）：
#       "info string Nuenagon eval: feature=<特徴セット> network=<ネットワーク> W=<W> L1=<L1> L2=<L2> eval_scale=<値> (file|option)"
#       "readyok"
#     失敗したら（ファイルが無い、マジック・形式バージョン・予約領域・長さ・CRC32 のどれかが不正、
#     特徴セットとネットワークと次元数の組み合わせが §F3.1 と合わない）、
#       "info string eval load error: <理由>"
#     を出し、readyok は出さない。エンジンは終了せず、次の命令を受け付ける。
#   eval                   → "eval raw <int> cp <int>"
#                            raw：最終出力の int32（§E4。z = raw / 8128、手番側から見た値）。
#                            cp：倍精度で round(raw × eval_scale / 8128)、0 から遠い方への四捨五入、クリップしない。
#                            eval_scale は EvalScale が 0 ならファイルの値、そうでなければ EvalScale の値。
#   acc <p>                → "acc <W 個の整数>"：視点 p（0 = 先手視点、1 = 後手視点）の第1層の蓄積値（int16、バイアス込み）。
#                            **差分更新で保っている値をそのまま**出す（作り直さない）。
#   accfull <p>            → "accfull <W 個の整数>"：同じものを、今の局面から全計算で作り直した値。
#   undo                   → 応答なし。直前の1手（null を含む）を戻す。蓄積値も戻す。
#   null                   → 応答なし。手番だけを入れ替える（null move、§E3.2）。王手がかかっていても実行してよい（テスト用）。
#   acccheck <手数> <乱数の種> → 自分でランダムに指し進め・戻し・null を混ぜながら、毎回「差分更新の蓄積値 ＝ 全計算」を確かめる。
#                            "acccheck ok <確かめた回数>" または "acccheck fail <SFEN> <直前の手>"。
#
# position の moves は、N1 と同じく1手ずつ差分の処理で進めること（蓄積値も差分更新する）。
# 量子化の掛け算（w × 127 など）は倍精度で行うこと（単精度で行うと、四捨五入の境目で結果が変わる）。
import json
import os
import struct
import sys
import zlib

import numpy as np
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "reference"))
sys.path.insert(0, HERE)
import m4_ref  # noqa: E402
import raypair_ref as ref  # noqa: E402

N2 = json.load(open(os.path.join(HERE, "golden", "n2_golden.json"), encoding="utf-8"))
NETS = {0: ("RayPair_v0", "RayPair_v0Network", 15000, 16), 1: ("RayPair_v1", "RayPair_v1Network", 115352, 32)}
LIM = 127 / 64


def header(v, scale=543.21, lam=0.75, positions=12345):
    f, n, nf, l = NETS[v]
    return {"feature_set": f, "network": n, "num_features": nf, "W": 128, "L1": l, "L2": l,
            "eval_scale": scale, "lam": lam, "positions": positions}


def make_weights(v, kind, seed):
    """kind：'random'（ふつうの乱数）／'extreme'（ft は全部 +1、隠れ層は上限・下限と境目の値）。"""
    _, _, nf, l = NETS[v]
    W = 128
    rng = np.random.default_rng(seed)
    if kind == "random":
        t = {"ft_weight": rng.uniform(-0.3, 0.3, (nf, W)), "ft_bias": rng.uniform(-1, 1, W),
             "l1_weight": rng.uniform(-LIM, LIM, (l, 2 * W)) * 0.3, "l1_bias": rng.uniform(-2, 2, l),
             "l2_weight": rng.uniform(-LIM, LIM, (l, l)), "l2_bias": rng.uniform(-1, 1, l),
             "out_weight": rng.uniform(-LIM, LIM, (1, l)), "out_bias": rng.uniform(-1, 1, 1)}
        # 隠れ層の重みに、×64 でちょうど .5 になる値を混ぜる（丸めの方向の確認）
        ties = np.array([0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 63.5, -63.5]) / 64
        t["l1_weight"][0, :8] = ties
        t["l2_weight"][1, :8 if l >= 8 else l] = ties[:min(8, l)]
    else:
        t = {"ft_weight": np.ones((nf, W)), "ft_bias": np.ones(W),
             "l1_weight": rng.choice([-LIM, LIM, 0.0], (l, 2 * W)), "l1_bias": rng.uniform(-300, 300, l),
             "l2_weight": rng.choice([-LIM, LIM], (l, l)), "l2_bias": rng.uniform(-50, 50, l),
             "out_weight": rng.choice([-LIM, LIM], (1, l)), "out_bias": rng.uniform(-5, 5, 1)}
        t["ft_weight"][:, ::2] = -1.0      # 半分の列は −1（負の飽和側も通す）
    return {k: np.asarray(x, dtype=np.float32) for k, x in t.items()}


@pytest.fixture(scope="module")
def shk_files(tmp_path_factory):
    d = tmp_path_factory.mktemp("shk")
    out = {}
    for v in (0, 1):
        for kind in ("random", "extreme"):
            t = make_weights(v, kind, seed=100 + v * 10 + (kind == "extreme"))
            h = header(v)
            p = str(d / f"v{v}_{kind}.shk")
            with open(p, "wb") as f:
                f.write(m4_ref.build_shk(h, t))
            rh, rt = m4_ref.parse_shk(open(p, "rb").read())   # 作ったファイルが仕様どおりであることの確認
            out[(v, kind)] = {"path": p, "header": rh, "q": m4_ref.quantize(rt)}
    return out


def load(eng, path, scale=0):
    eng.send(f"setoption name EvalFile value {path}")
    eng.send(f"setoption name EvalScale value {scale}")
    eng.send("isready")
    info = eng.expect("info string Nuenagon eval:", timeout=120)
    eng.expect("readyok", timeout=120)
    return info


def ints(line):
    return [int(t) for t in line.split()[1:]]


def expected(q, sfen, v, eval_scale):
    stm = 0 if sfen.split()[1] == "b" else 1
    fs, fn = ref.features(sfen, stm, v), ref.features(sfen, 1 - stm, v)
    a, b, raw = m4_ref.qforward(q, fs, fn)
    acc = {stm: [int(x) for x in a], 1 - stm: [int(x) for x in b]}
    return acc, raw, m4_ref.cp_of(raw, eval_scale)


def check_position(eng, f, v, sfen, label, scale=None):
    sc = f["header"]["eval_scale"] if scale is None else scale
    acc, raw, cp = expected(f["q"], sfen, v, sc)
    for p in (0, 1):
        got = ints(eng.ask(f"acc {p}", "acc"))
        assert got == acc[p], f"{label}: 視点{p}の蓄積値が違う（先頭 {got[:6]} / 期待 {acc[p][:6]}）"
        assert ints(eng.ask(f"accfull {p}", "accfull")) == acc[p], f"{label}: 視点{p}の全計算の蓄積値が違う"
    r = eng.ask("eval", "eval").split()
    assert r[1] == "raw" and r[3] == "cp"
    assert int(r[2]) == raw, f"{label}: raw が違う（{r[2]} / 期待 {raw}）"
    assert int(r[4]) == cp, f"{label}: cp が違う（{r[4]} / 期待 {cp}）"


# ---------- 読み込み ----------

@pytest.mark.parametrize("v", [0, 1])
def test_load_info(eng, shk_files, v):
    info = load(eng, shk_files[(v, "random")]["path"])
    f, n, nf, l = NETS[v]
    for s in (f"feature={f}", f"network={n}", "W=128", f"L1={l}", f"L2={l}", "(file)"):
        assert s in info, f"読み込み時の表示に {s} が無い: {info}"
    assert "eval_scale=543.2" in info


def _read_until(eng, prefix, timeout=60):
    """prefix で始まる行が来るまでの、全部の行を返す。"""
    lines = []
    while True:
        line = eng.q.get(timeout=timeout)
        assert line is not None, "エンジンが終了した"
        lines.append(line)
        if line == prefix or line.startswith(prefix + " "):
            return lines


def test_reject_corrupted(eng, shk_files, tmp_path):
    good = open(shk_files[(0, "random")]["path"], "rb").read()
    h, t = m4_ref.parse_shk(good)
    hb = dict(h)
    hb["num_features"] = 15001          # 特徴セットと次元数が合わない
    t2 = dict(t)
    t2["ft_weight"] = np.vstack([t["ft_weight"], np.zeros((1, 128), np.float32)])
    bad = {
        "crc": good[:2000] + bytes([good[2000] ^ 1]) + good[2001:],
        "magic": b"SHIKIBX\0" + good[8:],
        "version": good[:8] + struct.pack("<I", 2) + good[12:],
        "truncated": good[:-100],
        "reserved": good[:130] + b"\x07" + good[131:],
        "mismatch": m4_ref.build_shk(hb, t2),
    }
    for name, data in list(bad.items()) + [("missing", None)]:
        p = tmp_path / f"bad_{name}.shk"
        if data is not None:
            p.write_bytes(data)
        eng.send(f"setoption name EvalFile value {p}")
        eng.send("isready")
        lines = _read_until(eng, "info string eval load error:", timeout=60)
        assert "readyok" not in lines, f"{name}: 不正なファイルで readyok を返した"
        # 次の命令を受け付けること、遅れて readyok が来ないこと
        eng.send("sfen")
        lines = _read_until(eng, "sfen", timeout=30)
        assert "readyok" not in lines, f"{name}: 不正なファイルのあとで readyok を返した"
    load(eng, shk_files[(0, "random")]["path"])   # 正しいファイルなら、また読み込める


# ---------- 推論（全計算と差分更新） ----------

@pytest.mark.parametrize("v,kind", [(0, "random"), (0, "extreme"), (1, "random"), (1, "extreme")])
def test_eval_golden_positions(eng, shk_files, v, kind):
    f = shk_files[(v, kind)]
    load(eng, f["path"])
    eng.send("position startpos")
    check_position(eng, f, v, "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1", "平手初期局面")
    n = 120 if v == 0 else 40
    for i, x in enumerate(N2["positions"][:n]):
        eng.send("position startpos moves " + " ".join(x["moves"]))                   # 差分更新の道筋
        check_position(eng, f, v, x["sfen"] + " 1", f"#{i}（moves から）")
        if i % 4 == 0:
            eng.send(f"position sfen {x['sfen']} 1")                                    # 全計算の道筋
            check_position(eng, f, v, x["sfen"] + " 1", f"#{i}（sfen から）")


def test_eval_scale_option(eng, shk_files):
    f = shk_files[(0, "random")]
    info = load(eng, f["path"], scale=777)
    assert "eval_scale=777" in info and "(option)" in info
    x = N2["positions"][5]
    eng.send("position startpos moves " + " ".join(x["moves"]))
    check_position(eng, f, 0, x["sfen"] + " 1", "EvalScale=777", scale=777)
    # eval_scale = 4064（= 8128 / 2）なら、raw が奇数のとき cp がちょうど .5 になる。四捨五入の方向を確かめる
    load(eng, f["path"], scale=4064)
    odd = 0
    for i, x in enumerate(N2["positions"][:60]):
        eng.send("position startpos moves " + " ".join(x["moves"]))
        r = eng.ask("eval", "eval").split()
        raw, cp = int(r[2]), int(r[4])
        assert cp == m4_ref.cp_of(raw, 4064), f"#{i}: raw {raw} のとき cp {cp}（期待 {m4_ref.cp_of(raw, 4064)}）"
        odd += raw % 2
    assert odd >= 5, "raw が奇数の局面が少なすぎて、四捨五入の方向を確かめられない"
    load(eng, f["path"], scale=0)


# ---------- 戻す・null move ----------

def test_undo(eng, shk_files):
    f = shk_files[(0, "random")]
    load(eng, f["path"])
    for i, x in enumerate(N2["positions"][:40]):
        mv = x["moves"]
        if len(mv) < 4:
            continue
        k = 1 + i % 3
        eng.send("position startpos moves " + " ".join(mv))
        for _ in range(k):
            eng.send("undo")
        import cshogi
        b = cshogi.Board()
        for m in mv[:-k]:
            b.push_usi(m)
        check_position(eng, f, 0, b.sfen(), f"#{i} {k}手戻した後")


def test_null_move(eng, shk_files):
    f = shk_files[(0, "random")]
    load(eng, f["path"])
    import cshogi
    for i, x in enumerate(N2["positions"][:40]):
        eng.send("position startpos moves " + " ".join(x["moves"]))
        b = cshogi.Board()
        for m in x["moves"]:
            b.push_usi(m)
        s = b.sfen().split()
        flipped = f"{s[0]} {'w' if s[1] == 'b' else 'b'} {s[2]} 1"
        eng.send("null")
        check_position(eng, f, 0, flipped, f"#{i} null の後")
        eng.send("undo")
        check_position(eng, f, 0, b.sfen(), f"#{i} null を戻した後")


def test_acccheck(eng, shk_files):
    for v in (0, 1):
        load(eng, shk_files[(v, "extreme")]["path"])
        r = eng.ask("acccheck 100000 7" if v == 0 else "acccheck 20000 8", "acccheck", timeout=900)
        assert r.startswith("acccheck ok"), r
        assert int(r.split()[2]) >= (100000 if v == 0 else 20000)
