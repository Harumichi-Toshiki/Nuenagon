# 変更禁止（PROTECTED）。Nuenagon N5H（HalfKP 対応：やねうら王の halfkp_256x2-32-32 の評価関数ファイルを読んで使う）の受け入れテスト。
#
# 目的：同じ探索（Nuenagon）で RayPair と HalfKP を比べ、評価関数の構造の差を測れるようにする（設計書 §E10）。
# 特徴量・推論の定義は tests/halfkp_ref.py の冒頭のコメント（やねうら王と一致することを確かめてある）。
# 重みは tests/halfkp_nnbin.py でテストの中で作る（普通の幅と、層ごとに飽和しやすい幅）。
# tests/golden/halfkp_golden.json は、やねうら王（コミット 32165a8）が同じ重み・同じ局面で出した静的評価値。
#
# ■ 読み込み
#   EvalFile のファイルの先頭4バイトが 0x7AF32F16（リトルエンディアン）なら、やねうら王の NNUE 形式として読む。
#   それ以外は今までどおり .shk（RayPair）として読む。どちらも、同じエンジンの中で何度でも切り替えられる。
#   検査：版、ファイル全体のハッシュ（0x3E5AA6EE）、特徴変換のハッシュ（0x5D69D7B8）、ネットワークのハッシュ（0x63337156）、
#   ファイルの長さ（ちょうど最後まで読み切ること。短くても長くても不正）。
#   構造の文字列は、長さに従って読み飛ばす（中身は検査しない。学習器や版によって書き方が違うため。やねうら王も照合はハッシュで行う）。
#   成功：  "info string Nuenagon eval: feature=HalfKP network=halfkp_256x2-32-32 W=256 L1=32 L2=32 fv_scale=<値>"
#   失敗：  "info string eval load error: <理由>" を出し、readyok は出さない（N3 と同じ）。
#   USI オプション FV_SCALE（spin、既定 16、1〜128）。HalfKP のときだけ使う。
#
# ■ 命令（N3 と同じ命令を HalfKP でも）
#   eval      → "eval raw <int> cp <int>"。raw は出力層の int32、cp は raw / FV_SCALE を 0 の方向に切り捨てた値（クリップしない）。
#   acc <p>   → "acc <256 個の整数>"：視点 p の蓄積値（差分更新で保っている値をそのまま）。accfull <p> は全計算。
#   acccheck <手数> <種> → N3 と同じ（HalfKP でも、玉が動いたときの作り直しを含めて確かめる）。
#   evalbench → N3 と同じ（HalfKP の差分更新と評価の時間）。
#
# ■ 探索
#   HalfKP の評価値で探索できること。探索の中での評価値の扱い（cp の上限など）は RayPair と同じ。
#   DebugCheckAccumulator も HalfKP で働くこと。
#
# ■ ビルド
#   NUENAGON_SIMD=OFF のビルド（build_scalar/）でも、同じ値になること。
#   RayPair の動作（N1〜N5 のテスト。N5 の「N4.1 と探索の結果が同じ」を含む）は変えないこと。
import json
import os
import random
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import cshogi  # noqa: E402
import halfkp_nnbin as G  # noqa: E402
import halfkp_ref as R  # noqa: E402
from conftest import Engine  # noqa: E402

GOLD = json.load(open(os.path.join(HERE, "golden", "halfkp_golden.json"), encoding="utf-8"))
POS = GOLD["positions"]
N1 = json.load(open(os.path.join(HERE, "golden", "n1_golden.json"), encoding="utf-8"))
MATERIAL = os.path.join(HERE, "golden", "material_v0.shk")


@pytest.fixture(scope="module")
def nets(tmp_path_factory):
    d = tmp_path_factory.mktemp("halfkp")
    out = {}
    for mode, info in GOLD["files"].items():
        p = str(d / f"{mode}.bin")
        G.make(p, info["seed"], mode)
        import hashlib
        got = hashlib.sha256(open(p, "rb").read()).hexdigest()
        assert got == info["sha256"], f"{mode} の nn.bin が正解データを作ったときと違う（{got}）。作業を止めて報告すること"
        out[mode] = {"path": p, "net": R.Net(p)}
    return out


def load(eng, path, fv=16):
    eng.send(f"setoption name FV_SCALE value {fv}")
    eng.send(f"setoption name EvalFile value {path}")
    eng.send("isready")
    info = eng.expect("info string Nuenagon eval:", timeout=180)
    eng.expect("readyok", timeout=180)
    return info


def ints(line):
    return [int(t) for t in line.split()[1:]]


def eval_line(eng):
    r = eng.ask("eval", "eval").split()
    assert r[1] == "raw" and r[3] == "cp", f"eval の形式が違う: {r}"
    return int(r[2]), int(r[4])


# ---------------------------------------------------------------- 読み込み

def test_load_info(eng, nets):
    info = load(eng, nets["normal"]["path"])
    for k in ("feature=HalfKP", "network=halfkp_256x2-32-32", "W=256", "L1=32", "L2=32", "fv_scale=16"):
        assert k in info, f"読み込みの info に {k} が無い: {info}"
    info = load(eng, nets["normal"]["path"], fv=24)
    assert "fv_scale=24" in info, info


def _read_until_ready_or_error(eng, timeout=180):
    import queue
    import time
    end = time.time() + timeout
    while time.time() < end:
        try:
            line = eng.q.get(timeout=max(0.1, end - time.time()))
        except queue.Empty:
            continue
        if line is None:
            raise RuntimeError("エンジンが終了した")
        if line.startswith("info string eval load error"):
            return "error", line
        if line == "readyok":
            return "ready", line
    raise TimeoutError("isready の応答が無い")


def test_reject_corrupted(eng, nets, tmp_path):
    good = open(nets["normal"]["path"], "rb").read()
    n = struct.unpack_from("<I", good, 8)[0]
    ft_at = 12 + n
    net_at = ft_at + 4 + 256 * 2 + R.N_FEATURES * 256 * 2

    def patched(off, val):
        b = bytearray(good)
        struct.pack_into("<I", b, off, val)
        return bytes(b)

    bad = {
        "version": patched(0, 0x7AF32F17),
        "file_hash": patched(4, 0x12345678),
        "ft_hash": patched(ft_at, 0x12345678),
        "net_hash": patched(net_at, 0x12345678),
        "short": good[:-1],
        "long": good + b"\x00",
    }
    for name, data in bad.items():
        p = tmp_path / f"bad_{name}.bin"
        p.write_bytes(data)
        eng.send(f"setoption name EvalFile value {p}")
        eng.send("isready")
        kind, line = _read_until_ready_or_error(eng)
        assert kind == "error", f"壊れた nn.bin（{name}）を読み込んでしまった"
    info = load(eng, nets["normal"]["path"])
    assert "feature=HalfKP" in info


def test_other_architecture_string(eng, nets, tmp_path):
    """構造の文字列の書き方が違う（AffineTransformSparseInput ではなく AffineTransform）ファイルも、同じように読めて同じ値になる。"""
    good = open(nets["normal"]["path"], "rb").read()
    n = struct.unpack_from("<I", good, 8)[0]
    alt = G.ARCH.replace("AffineTransformSparseInput", "AffineTransform").encode()
    p = tmp_path / "alt_arch.bin"
    p.write_bytes(good[:8] + struct.pack("<I", len(alt)) + alt + good[12 + n:])
    load(eng, str(p))
    check_positions(eng, nets["normal"], POS[::20], "alt_arch", acc_every=4)


def test_switch_between_raypair_and_halfkp(eng, nets):
    info = load(eng, MATERIAL)
    assert "feature=RayPair_v0" in info, info
    info = load(eng, nets["normal"]["path"])
    assert "feature=HalfKP" in info, info
    info = load(eng, MATERIAL)
    assert "feature=RayPair_v0" in info, info


# ---------------------------------------------------------------- 評価値の一致

def check_positions(eng, f, sfens, label, fv=16, acc_every=10):
    for i, s in enumerate(sfens):
        eng.send(f"position sfen {s}")
        b = cshogi.Board(s)
        acc, raw, cp = f["net"].forward(b, fv)
        got_raw, got_cp = eval_line(eng)
        assert got_raw == raw, f"{label}: raw が違う（{got_raw} / 期待 {raw}）: {s}"
        assert got_cp == cp, f"{label}: cp が違う（{got_cp} / 期待 {cp}）: {s}"
        if i % acc_every == 0:
            for p in (0, 1):
                a = ints(eng.ask(f"acc {p}", "acc"))
                assert a == [int(x) for x in acc[p]], f"{label}: 視点{p}の蓄積値が違う（先頭 {a[:6]} / 期待 {list(acc[p][:6])}）: {s}"
                assert ints(eng.ask(f"accfull {p}", "accfull")) == a, f"{label}: 視点{p}の全計算が差分の値と違う: {s}"


@pytest.mark.parametrize("mode", ["normal", "wide_ft", "wide_l1", "wide_l2", "wide_out"])
def test_eval_matches_yaneuraou(eng, nets, mode):
    f = nets[mode]
    load(eng, f["path"])
    # まず、参照実装がやねうら王の値と一致していることを確かめる（テストの前提）
    ref_cp = [f["net"].forward(cshogi.Board(s), 16)[2] for s in POS]
    assert ref_cp == GOLD["files"][mode]["cp"]["16"], "参照実装とやねうら王の値が違う（テストの前提が崩れている）。作業を止めて報告すること"
    check_positions(eng, f, POS, mode)


def test_fv_scale_option(eng, nets):
    f = nets["normal"]
    load(eng, f["path"], fv=24)
    for s, want in list(zip(POS, GOLD["files"]["normal"]["cp"]["24"]))[::5]:
        eng.send(f"position sfen {s}")
        assert eval_line(eng)[1] == want, f"FV_SCALE=24 の cp が違う: {s}"
    load(eng, f["path"], fv=16)


def test_incremental_along_games(eng, nets):
    """初手から1手ずつ差分更新で進めた局面（玉の移動・駒取り・成り・駒打ちを含む）でも、値が全計算と参照実装に一致する。"""
    for mode in ("normal", "wide_ft"):
        f = nets[mode]
        load(eng, f["path"])
        for x in N1["random_positions"][::8]:
            eng.send("position startpos moves " + " ".join(x["moves"]))
            b = cshogi.Board()
            for m in x["moves"]:
                b.push_usi(m)
            acc, raw, cp = f["net"].forward(b)
            assert eval_line(eng) == (raw, cp), f"{mode}: 差分更新で進めた局面の評価値が違う: {b.sfen()}"
            for p in (0, 1):
                a = ints(eng.ask(f"acc {p}", "acc"))
                assert a == [int(v) for v in acc[p]], f"{mode}: 差分更新で進めた局面の、視点{p}の蓄積値が違う: {b.sfen()}"


@pytest.mark.parametrize("mode", ["normal", "wide_ft"])
def test_acccheck(eng, nets, mode):
    load(eng, nets[mode]["path"])
    for seed in (1, 2, 3):
        eng.send("position startpos")
        r = eng.ask(f"acccheck 20000 {seed}", "acccheck", timeout=300)
        assert r.startswith("acccheck ok"), f"{mode}: 差分更新が全計算と合わない: {r}"


# ---------------------------------------------------------------- 探索

def _search(eng, pos, go, timeout=120):
    eng.send(pos)
    eng.send(go)
    infos = []
    while True:
        line = eng.q.get(timeout=timeout)
        assert line is not None, "エンジンが終了した"
        if line.startswith("info depth "):
            infos.append(line)
        if line.startswith("info string accumulator mismatch"):
            raise AssertionError(f"探索中の差分更新の検査で不一致: {line}")
        if line.startswith("bestmove"):
            return infos, line


def _strip_time(line):
    t = line.split()
    out = []
    skip = False
    for i, w in enumerate(t):
        if skip:
            skip = False
            continue
        if w in ("time", "nps", "hashfull"):
            skip = True
            continue
        out.append(w)
    return " ".join(out)


def test_search_deterministic_and_checked(eng, nets):
    load(eng, nets["normal"]["path"])
    for k, v in (("Threads", 1), ("Deterministic", "true"), ("DebugCheckAccumulator", "true"), ("MultiPV", 1)):
        eng.send(f"setoption name {k} value {v}")
    eng.send("isready")
    eng.expect("readyok", timeout=180)
    for x in N1["random_positions"][40:400:60]:
        pos = "position startpos moves " + " ".join(x["moves"])
        runs = []
        for _ in range(2):
            eng.send("usinewgame")
            infos, best = _search(eng, pos, "go nodes 30000")
            runs.append(([_strip_time(i) for i in infos], best))
        assert runs[0] == runs[1], f"決定的モードで、HalfKP の探索の結果が毎回違う: {pos}"
        b = cshogi.Board()
        for m in x["moves"]:
            b.push_usi(m)
        bm = runs[0][1].split()[1]
        if bm not in ("resign", "win"):
            assert b.is_legal(b.move_from_usi(bm)), f"非合法手 {bm}: {pos}"
        for line in runs[0][0]:
            t = line.split()
            if "cp" in t:
                assert abs(int(t[t.index("cp") + 1])) < 30000, f"cp に詰みの値が出ている: {line}"
    for k, v in (("Deterministic", "false"), ("DebugCheckAccumulator", "false")):
        eng.send(f"setoption name {k} value {v}")


def test_search_finds_mate(eng, nets):
    load(eng, nets["normal"]["path"])
    eng.send("setoption name Threads value 1")
    cases = [x for x in N1["mate"] if x["length"] <= 5][:8]
    for x in cases:
        eng.send("usinewgame")
        infos, best = _search(eng, f"position sfen {x['sfen']}", "go movetime 2000")
        last = infos[-1].split()
        assert "mate" in last and int(last[last.index("mate") + 1]) > 0, f"HalfKP で詰みを見つけていない: {x['sfen']} / {infos[-1]}"


# ---------------------------------------------------------------- SIMD を使わないビルド

@pytest.fixture(scope="module")
def scalar_exe():
    bd = os.path.join(ROOT, "build_scalar")
    subprocess.run(["cmake", "-S", ROOT, "-B", bd, "-DCMAKE_BUILD_TYPE=Release", "-DNUENAGON_SIMD=OFF"], check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", bd, "-j"], check=True, stdout=subprocess.DEVNULL)
    for c in ("nuenagon", "Release/nuenagon.exe", "nuenagon.exe"):
        p = os.path.join(bd, c)
        if os.path.exists(p):
            return p
    raise FileNotFoundError("build_scalar に nuenagon が無い")


def test_scalar_build_matches(nets, scalar_exe):
    old = os.environ.get("NUENAGON_BIN")
    os.environ["NUENAGON_BIN"] = scalar_exe
    try:
        e = Engine()
        try:
            for mode in ("normal", "wide_ft", "wide_l1"):
                load(e, nets[mode]["path"])
                check_positions(e, nets[mode], POS[::4], f"scalar/{mode}", acc_every=5)
                e.send("position startpos")
                r = e.ask("acccheck 5000 7", "acccheck", timeout=300)
                assert r.startswith("acccheck ok"), f"scalar/{mode}: {r}"
        finally:
            e.close()
    finally:
        if old is None:
            os.environ.pop("NUENAGON_BIN", None)
        else:
            os.environ["NUENAGON_BIN"] = old


# ---------------------------------------------------------------- 参考の記録（合否には使わない）

def test_report_evalbench(eng, nets, capsys):
    lines = []
    for path in (MATERIAL, nets["normal"]["path"]):
        load(eng, path)
        eng.send("position sfen ln6l/2s6/r5ns1/5k1pp/PG3pP2/9/1PPG1KNPP/7+bL/LNSGGS3 w 2Prb7p 68")
        lines.append(os.path.basename(path) + ": " + eng.ask("evalbench 200000", "info string evalbench", timeout=300))
    with capsys.disabled():
        print("\n" + "\n".join(lines))
