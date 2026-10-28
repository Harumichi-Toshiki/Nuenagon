# 変更禁止（PROTECTED）。Nuenagon N6（Dajo プロトコル DJP v0、設計書 §E9）の受け入れテスト。
#
# 合議に要る構成と、ponder のための promote、評価関数を呼ぶ eval、手ごとの正確な評価 rank を確かめる。
#   必須の caps：search, rank, probe, threat, mate, eval, promote, sysinfo
#   必須の制御：cancel, set, sync, newgame
# hint・watch は今回は必須ではない（作ったなら caps に書く。書いたものは §E9 のとおりに動くこと）。
#
# 書式・応答の形・エラーのコードは、すべて設計書 §E9 のとおり。テストが特に確かめること：
#   - dajo hello を受け取るまで、"dajo" で始まる行を一切出さない（§E9.1）。
#   - ジョブは accepted → （queued →）started → （move・info・event …）→ result の順。result は各ジョブに 1 回だけ。
#     そのジョブの行は、すべてそのジョブの番号を持つ。
#   - 複数のジョブを同時に動かせる。それぞれのジョブは自分の局面だけを読む（§E9.1 の 4）。
#   - move の行は move・score・wr・bound・depth・seldepth・nodes・pv のキーを持つ。
#     wr は 1 / (1 + exp(−score / eval_scale))（eval_scale は isready の info の値）と ±0.01 で一致する。詰みなら mate キーを付け、wr は勝ち 1・負け 0。
#   - search の result は bestmove（合法手）を持つ。exclude した手は bestmove にも move の行にも出さない。
#     searchmoves を指定したら、その手だけを読む。multipv=k なら、反復ごとに k 手の move を出す。
#   - rank（§E9.4）：result の直前に、指定した手（moves を省いたら全合法手）それぞれの move の行を 1 行ずつ出す。
#     その行はすべて同じ depth で bound=exact（最後に全部の手が終わった段の値）。指定していない手の move の行は出さない。
#     途中の段の move の行は出してよい（report=iter）。値は手番側から見た cp なので、良い手ほど score が大きい。
#     moves に非合法手があれば illegal_move。
#   - エラー：上限なし → no_limit、局面の書き方が不正 → illegal_position、moves に非合法手 → illegal_move、
#     動いているジョブと同じ番号 → duplicate_id、無い番号の cancel → no_such_job、王手の局面での threat → in_check、
#     知らない命令 → unknown_command（番号が無い命令では 0）、set の値が不正 → bad_arg（key=…）。
#   - 知らないキーは無視する（set の知らないキーも set ok）。
#   - 決定的モード（dajo set deterministic=1）では、同じ search のジョブ（nodes で制限）が毎回まったく同じ move の行と
#     result を返す（time・nps・hashfull を除く）。
#   - DJP を使った後も、USI の go はふつうに動き、"dajo" で始まる行を出さない（番号 0 は USI の go）。
#
# 評価関数は tests/golden/material_v0.shk（駒の損得。タダで駒を取られる手の判定がはっきりしているため）。
import math
import os
import queue
import sys
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cshogi  # noqa: E402
from conftest import Engine  # noqa: E402
import json  # noqa: E402

SHK = os.path.join(HERE, "golden", "material_v0.shk")
N1 = json.load(open(os.path.join(HERE, "golden", "n1_golden.json"), encoding="utf-8"))
REQUIRED_CAPS = {"search", "rank", "probe", "threat", "mate", "eval", "promote", "sysinfo"}

# 先手の飛車 5h が、後手の金 5c と同じ筋にいる。5h5c は金をタダで取る。5h5d は金に取られる（飛車をタダで捨てる）。
BLUNDER = "4k4/9/4g4/9/9/9/9/4R4/4K4 b - 1"
# 先手番。先手がパスすると、後手の G*5h で詰む（詰めろ）。
TSUMERO = "4k4/9/9/9/9/9/4g4/9/4K4 b g 1"
# 先手番で、先手の玉に王手がかかっている。
IN_CHECK = "4k4/9/9/9/9/9/9/4r4/4K4 b - 1"


# ---------------------------------------------------------------- 道具

class Djp:
    def __init__(self, threads=2):
        self.e = Engine()
        self.buf = []
        self.e.send("usi")
        self.e.expect("usiok", timeout=30)
        for k, v in (("EvalFile", SHK), ("Threads", threads), ("USI_Hash", 64), ("NetworkDelay", 0),
                     ("USI_Ponder", "false"), ("Deterministic", "false"), ("MultiPV", 1)):
            self.e.send(f"setoption name {k} value {v}")
        self.e.send("isready")
        self.ready_lines = self.read_until(lambda l: l == "readyok", timeout=180)
        info = [l for l in self.ready_lines if "eval_scale=" in l]
        self.eval_scale = float(info[0].split("eval_scale=")[1].split()[0]) if info else 600.0

    def send(self, s):
        self.e.send(s)

    def readline(self, timeout):
        try:
            line = self.e.q.get(timeout=timeout)
        except queue.Empty:
            return None
        if line is None:
            raise RuntimeError("エンジンが終了した")
        return line

    def read_until(self, pred, timeout=60):
        out = []
        end = time.time() + timeout
        while True:
            rest = end - time.time()
            if rest <= 0:
                raise TimeoutError(f"待っている応答が {timeout} 秒以内に来ない。それまでの行: {out[-8:]}")
            line = self.readline(min(rest, 0.5))
            if line is None:
                continue
            out.append(line)
            if pred(line):
                return out

    def collect(self, seconds):
        """seconds 秒のあいだに来た行を全部返す。"""
        out = []
        end = time.time() + seconds
        while time.time() < end:
            line = self.readline(max(0.01, end - time.time()))
            if line is not None:
                out.append(line)
        return out

    def hello(self):
        self.send("dajo hello 0")
        return self.read_until(lambda l: l.startswith("dajo hello"), timeout=10)[-1]

    def job(self, jid, text, timeout=120):
        """ジョブを投げ、その result の行までの全部の行を返す。"""
        self.send(f"dajo job {jid} {text}")
        return self.read_until(lambda l: l.startswith(f"dajo result {jid} ") or l.startswith(f"dajo error {jid} "),
                               timeout=timeout)

    def close(self):
        self.e.close()


def kv(line):
    d = {}
    for t in line.split():
        if "=" in t:
            k, v = t.split("=", 1)
            d[k] = v
    return d


def of_job(lines, jid, kind=None):
    out = []
    for l in lines:
        t = l.split()
        if len(t) >= 3 and t[0] == "dajo" and t[2] == str(jid) and (kind is None or t[1] == kind):
            out.append(l)
    return out


def board_of(pos):
    t = pos.split()
    if t[0] == "startpos":
        b = cshogi.Board()
        rest = t[1:]
    else:
        assert t[0] == "sfen"
        b = cshogi.Board(" ".join(t[1:5]))
        rest = t[5:]
    if rest[:1] == ["moves"]:
        for u in rest[1:]:
            b.push_usi(u)
    return b


def legal(b, u):
    try:
        return b.is_legal(b.move_from_usi(u))
    except Exception:
        return False


def check_move_line(d, line, eval_scale):
    for k in ("move", "score", "wr", "bound", "depth", "seldepth", "nodes", "pv"):
        assert k in d, f"move の行に {k} が無い: {line}"
    assert d["bound"] in ("exact", "lower", "upper"), line
    wr = float(d["wr"])
    if "mate" in d:
        m = int(d["mate"].replace("+", ""))
        assert (wr == 1.0) if m > 0 else (wr == 0.0), f"詰みの wr が 1／0 でない: {line}"
    else:
        want = 1.0 / (1.0 + math.exp(-int(d["score"]) / eval_scale))
        assert abs(wr - want) <= 0.01, f"wr が score から計算した値（{want:.3f}）と合わない: {line}"


@pytest.fixture
def djp():
    d = Djp()
    yield d
    d.close()


# ---------------------------------------------------------------- 有効化

def test_silent_before_hello():
    d = Djp()
    try:
        lines = list(d.ready_lines)
        d.send("position startpos")
        d.send("go nodes 20000")
        lines += d.read_until(lambda l: l.startswith("bestmove"), timeout=60)
        bad = [l for l in lines if l.startswith("dajo")]
        assert not bad, f"dajo hello の前に dajo の行を出した: {bad[:3]}"
    finally:
        d.close()


def test_hello_and_caps(djp):
    line = djp.hello()
    t = line.split()
    assert t[:3] == ["dajo", "hello", "0"], line
    d = kv(line)
    assert d.get("engine") == "Nuenagon", line
    caps = set(d.get("caps", "").split(","))
    assert REQUIRED_CAPS <= caps, f"caps に必要な機能が無い: {sorted(REQUIRED_CAPS - caps)}（{line}）"
    djp.send("dajo hello 3")
    line = djp.read_until(lambda l: l.startswith("dajo hello"), timeout=10)[-1]
    assert line.split()[:3] == ["dajo", "hello", "0"], f"版 3 の要求に、対応する最大の版 0 を返していない: {line}"


def test_unknown_command_and_keys(djp):
    djp.hello()
    djp.send("dajo frobnicate")
    line = djp.read_until(lambda l: l.startswith("dajo error"), timeout=10)[-1]
    assert kv(line).get("code") == "unknown_command", line
    djp.send("dajo set no_such_key=1")
    line = djp.read_until(lambda l: l.startswith("dajo set") or l.startswith("dajo error"), timeout=10)[-1]
    assert line == "dajo set ok", f"知らないキーを無視していない: {line}"
    djp.send("dajo set threads=abc")
    line = djp.read_until(lambda l: l.startswith("dajo set") or l.startswith("dajo error"), timeout=10)[-1]
    d = kv(line)
    assert line.startswith("dajo error 0 ") and d.get("code") == "bad_arg" and d.get("key") == "threads", line


def test_sysinfo_set_sync(djp):
    djp.hello()
    djp.send("dajo sysinfo")
    line = djp.read_until(lambda l: l.startswith("dajo sysinfo"), timeout=10)[-1]
    d = kv(line)
    for k in ("logical", "physical", "simd", "threads", "affinity"):
        assert k in d, f"sysinfo に {k} が無い: {line}"
    djp.send("dajo set threads=2 hash=32 draw_value=0")
    assert djp.read_until(lambda l: l.startswith("dajo set") or l.startswith("dajo error"), timeout=30)[-1] == "dajo set ok"
    djp.send("dajo newgame")
    djp.send("dajo sync")
    djp.read_until(lambda l: l == "dajo synced", timeout=30)


# ---------------------------------------------------------------- search

def test_search_job(djp):
    djp.hello()
    lines = djp.job(1, "search nodes=150000 position startpos moves 7g7f 3c3d")
    mine = of_job(lines, 1)
    kinds = [l.split()[1] for l in mine]
    assert kinds[0] == "accepted", f"最初の行が accepted でない: {mine[:3]}"
    assert "started" in kinds, "started が無い"
    assert kinds.index("started") < kinds.index("result"), "started より前に result が来た"
    assert kinds.count("result") == 1
    moves = of_job(lines, 1, "move")
    assert moves, "move の行が無い"
    b = board_of("startpos moves 7g7f 3c3d")
    for l in moves:
        d = kv(l)
        check_move_line(d, l, djp.eval_scale)
        assert legal(b, d["move"]), f"非合法な手: {l}"
    res = kv(of_job(lines, 1, "result")[0])
    assert res.get("status") == "done", res
    for k in ("time", "nodes", "bestmove"):
        assert k in res, f"result に {k} が無い: {res}"
    assert legal(b, res["bestmove"]), res
    # result の後に、このジョブの行が出ないこと
    later = of_job(djp.collect(1.0), 1)
    assert not later, f"result の後にもジョブ 1 の行が出た: {later[:3]}"


def test_search_multipv_exclude_searchmoves(djp):
    djp.hello()
    pos = "startpos moves 7g7f 3c3d"
    b = board_of(pos)
    lines = djp.job(2, f"search nodes=200000 multipv=3 position {pos}")
    moves = of_job(lines, 2, "move")
    by_depth = {}
    for l in moves:
        d = kv(l)
        by_depth.setdefault(int(d["depth"]), set()).add(d["move"])
    full = [s for s in by_depth.values() if len(s) >= 3]
    assert full, f"multipv=3 で、3 手そろった反復が無い: { {k: sorted(v) for k, v in by_depth.items()} }"
    best = kv(of_job(lines, 2, "result")[0])["bestmove"]
    lines = djp.job(3, f"search nodes=150000 exclude={best} position {pos}")
    res = kv(of_job(lines, 3, "result")[0])
    assert res["bestmove"] != best, f"exclude した手 {best} を bestmove にした"
    assert all(kv(l)["move"] != best for l in of_job(lines, 3, "move")), "exclude した手の move の行を出した"
    pick = "1g1f" if best != "1g1f" else "9g9f"
    assert legal(b, pick)
    lines = djp.job(4, f"search nodes=100000 searchmoves={pick} position {pos}")
    res = kv(of_job(lines, 4, "result")[0])
    assert res["bestmove"] == pick, f"searchmoves={pick} なのに bestmove が {res['bestmove']}"


def test_job_errors(djp):
    djp.hello()
    cases = [
        (31, "search position startpos", "no_limit"),
        (32, "search nodes=1000 position sfen thisisnotasfen b - 1", "illegal_position"),
        (33, "search nodes=1000 position startpos moves 7g7f 7g7f", "illegal_move"),
        (34, f"threat nodes=10000 position sfen {IN_CHECK}", "in_check"),
    ]
    for jid, text, code in cases:
        lines = djp.job(jid, text, timeout=30)
        err = of_job(lines, jid, "error")
        assert err and kv(err[0]).get("code") == code, f"{text} → {code} のはずが: {lines[-3:]}"
    djp.send("dajo cancel 999")
    line = djp.read_until(lambda l: l.startswith("dajo error 999") or l.startswith("dajo result 999"), timeout=10)[-1]
    assert kv(line).get("code") == "no_such_job", line
    djp.send("dajo job 35 search infinite=1 threads=1 position startpos")
    djp.read_until(lambda l: l.startswith("dajo started 35"), timeout=30)
    djp.send("dajo job 35 search nodes=1000 position startpos")
    line = djp.read_until(lambda l: l.startswith("dajo error 35"), timeout=10)[-1]
    assert kv(line).get("code") == "duplicate_id", line
    djp.send("dajo cancel 35")
    line = djp.read_until(lambda l: l.startswith("dajo result 35"), timeout=10)[-1]
    assert kv(line).get("status") == "cancelled", line


# ---------------------------------------------------------------- 同時に動くジョブ・cancel・promote

def test_concurrent_jobs(djp):
    djp.hello()
    djp.send("dajo set threads=2")
    djp.read_until(lambda l: l.startswith("dajo set"), timeout=30)
    djp.send("dajo job 41 search infinite=1 threads=1 position startpos")
    djp.read_until(lambda l: l.startswith("dajo started 41"), timeout=30)
    lines = djp.job(42, f"search nodes=100000 threads=1 position sfen {BLUNDER}", timeout=60)
    assert not of_job(lines, 41, "result"), "ジョブ 42 が終わる前に、無期限のジョブ 41 が終わった"
    b42 = board_of(f"sfen {BLUNDER}")
    for l in of_job(lines, 42, "move"):
        assert legal(b42, kv(l)["move"]), f"ジョブ 42 の move がその局面の合法手でない（局面の取り違え）: {l}"
    assert legal(b42, kv(of_job(lines, 42, "result")[0])["bestmove"])
    b41 = cshogi.Board()
    for l in of_job(lines, 41, "move"):
        assert legal(b41, kv(l)["move"]), f"ジョブ 41 の move がその局面の合法手でない（局面の取り違え）: {l}"
    djp.send("dajo cancel 41")
    tail = djp.read_until(lambda l: l.startswith("dajo result 41"), timeout=10)
    res = kv(tail[-1])
    assert res.get("status") == "cancelled", res
    assert legal(b41, res["bestmove"]), res
    later = of_job(djp.collect(1.0), 41, "result")
    assert not later, "ジョブ 41 の result が 2 回出た"


def test_cancel_all(djp):
    djp.hello()
    for jid in (51, 52):
        djp.send(f"dajo job {jid} search infinite=1 threads=1 position startpos")
    djp.read_until(lambda l: l.startswith("dajo accepted 52"), timeout=30)
    time.sleep(0.5)
    djp.send("dajo cancel all")
    got = set()
    end = time.time() + 15
    while got != {"51", "52"} and time.time() < end:
        line = djp.readline(0.5)
        if line and line.startswith("dajo result "):
            assert kv(line).get("status") == "cancelled", line
            got.add(line.split()[2])
    assert got == {"51", "52"}, f"cancel all で止まったジョブ: {got}"


def test_promote(djp):
    djp.hello()
    djp.send("dajo job 61 search infinite=1 position startpos moves 2g2f 8c8d")
    djp.read_until(lambda l: l.startswith("dajo started 61"), timeout=30)
    time.sleep(0.3)
    t0 = time.time()
    djp.send("dajo promote 61 time=300")
    lines = djp.read_until(lambda l: l.startswith("dajo result 61"), timeout=10)
    assert any(l == "dajo promoted 61" for l in lines), "dajo promoted 61 が無い"
    assert time.time() - t0 <= 3.0, f"promote time=300 から result まで {time.time() - t0:.2f} 秒"
    assert kv(lines[-1]).get("status") == "done", lines[-1]


# ---------------------------------------------------------------- rank

def rank_final(lines, jid, k):
    """result の直前の k 行の move を返す（{手: kv}）。その k 行の間に、ほかの種類のこのジョブの行は無いこと。"""
    mine = [l for l in of_job(lines, jid) if l.split()[1] in ("move", "result")]
    assert mine and mine[-1].split()[1] == "result", f"rank の result が無い: {lines[-3:]}"
    tail = mine[-1 - k:-1]
    assert len(tail) == k and all(l.split()[1] == "move" for l in tail), \
        f"result の直前に、指定した {k} 手ぶんの move の行が無い: {mine[-1 - k:]}"
    fin = {kv(l)["move"]: (kv(l), l) for l in tail}
    assert len(fin) == k, f"result の直前の {k} 行に、同じ手が 2 回ある（最後の段の値がそろっていない）: {tail}"
    return fin


def test_rank_job(djp):
    djp.hello()
    pos = f"sfen {BLUNDER}"
    b = board_of(pos)
    # 指定した 3 手：金をタダで取る 5h5c、安全な 5h6h、飛車をタダで捨てる 5h5d
    want = ["5h5c", "5h6h", "5h5d"]
    lines = djp.job(141, f"rank moves={','.join(want)} nodes=300000 position {pos}")
    res = kv(of_job(lines, 141, "result")[0])
    assert res.get("status") == "done", res
    for l in of_job(lines, 141, "move"):
        assert kv(l)["move"] in want, f"指定していない手の move の行を出した: {l}"
    fin = rank_final(lines, 141, len(want))
    assert set(fin) == set(want), f"result の直前の move の行が、指定した手と一致しない: {sorted(fin)}"
    depths = {int(d["depth"]) for d, _ in fin.values()}
    assert len(depths) == 1, f"rank の最後の値の depth がそろっていない: {[l for _, l in fin.values()]}"
    for d, l in fin.values():
        check_move_line(d, l, djp.eval_scale)
        assert d["bound"] == "exact", f"rank の最後の値が exact でない: {l}"
    s = {m: int(fin[m][0]["score"]) for m in want}
    assert s["5h5c"] > s["5h6h"] > s["5h5d"], f"rank の値の並びがおかしい（5h5c > 5h6h > 5h5d のはず）: {s}"
    # moves を省いたら全合法手
    lines = djp.job(142, f"rank nodes=400000 position {pos}")
    allm = sorted(cshogi.move_to_usi(m) for m in b.legal_moves)
    fin = rank_final(lines, 142, len(allm))
    assert sorted(fin) == allm, f"moves を省いた rank が全合法手を返さない: {sorted(fin)}"
    assert len({int(d["depth"]) for d, _ in fin.values()}) == 1, "全合法手の rank で depth がそろっていない"
    assert all(d["bound"] == "exact" for d, _ in fin.values()), "全合法手の rank に exact でない値がある"
    worst = min(fin, key=lambda m: int(fin[m][0]["score"]))
    assert worst == "5h5d", f"いちばん悪い手が 5h5d（飛車をタダで捨てる）でない: {worst}"
    # 非合法な手
    lines = djp.job(143, f"rank moves=5h5c,5h4g nodes=10000 position {pos}", timeout=30)
    err = of_job(lines, 143, "error")
    assert err and kv(err[0]).get("code") == "illegal_move", f"rank の moves に非合法手があるのに illegal_move を返さない: {lines[-3:]}"


# ---------------------------------------------------------------- probe・threat・mate・eval

def test_probe_verdicts(djp):
    djp.hello()
    pos = f"sfen {BLUNDER}"
    lines = djp.job(71, f"probe move=5h5d threshold=0 nodes=300000 position {pos}")
    res = kv(of_job(lines, 71, "result")[0])
    assert res.get("verdict") == "no", f"飛車をタダで捨てる手が no にならない: {res}"
    assert res.get("pv", "").split(",")[0] == "5c5d", f"咎める手順が金で飛車を取る手で始まらない: {res}"
    for k in ("threshold", "score", "bound", "depth"):
        assert k in res, f"probe の result に {k} が無い: {res}"
    lines = djp.job(72, f"probe move=5h6h threshold=0 nodes=300000 position {pos}")
    res = kv(of_job(lines, 72, "result")[0])
    assert res.get("verdict") == "yes", f"安全な手が yes にならない: {res}"
    lines = djp.job(73, f"probe move=5h5c margin=300 nodes=300000 position {pos}")
    res = kv(of_job(lines, 73, "result")[0])
    assert res.get("verdict") == "yes", f"最善手（金をタダで取る）が margin=300 で yes にならない: {res}"
    lines = djp.job(74, f"probe move=5h5d margin=300 nodes=300000 position {pos}")
    res = kv(of_job(lines, 74, "result")[0])
    assert res.get("verdict") == "no", f"飛車を捨てる手が margin=300 で no にならない: {res}"
    lines = djp.job(75, f"probe move=5h5d threshold=0 nodes=30 mindepth=4 position {pos}")
    res = kv(of_job(lines, 75, "result")[0])
    assert res.get("verdict") == "unknown", f"mindepth に届かないのに結論を出した: {res}"


def test_threat_tsumero(djp):
    djp.hello()
    lines = djp.job(81, f"threat compare=1 tsumero=1 nodes=300000 position sfen {TSUMERO}")
    res = kv(of_job(lines, 81, "result")[0])
    assert res.get("status") == "done", res
    assert res.get("threat_move") == "G*5h", f"パスしたときの相手の最善手が G*5h でない: {res}"
    assert res.get("tsumero") == "yes", f"詰めろを yes と判定しない: {res}"
    assert res.get("mate_pv", "").split(",")[0] == "G*5h", res
    for k in ("score_pass", "score_normal", "gain"):
        assert k in res, f"threat の result に {k} が無い: {res}"
    assert int(res["gain"]) == int(res["score_normal"]) - int(res["score_pass"]), res


def mate_pv_ok(sfen, pv):
    b = cshogi.Board(sfen)
    for u in pv:
        if not legal(b, u):
            return False
        b.push_usi(u)
    return b.is_game_over()


def test_mate_job(djp):
    djp.hello()
    cases = [x for x in N1["mate"] if x["length"] <= 7][:8]
    assert len(cases) == 8
    for i, x in enumerate(cases):
        lines = djp.job(90 + i, f"mate side=self nodes=500000 position sfen {x['sfen']}")
        res = kv(of_job(lines, 90 + i, "result")[0])
        assert res.get("mate") == "yes", f"{x['length']} 手詰めで mate=yes にならない: {res}（{x['sfen']}）"
        pv = res.get("pv", "").split(",")
        assert int(res["length"]) % 2 == 1 and int(res["length"]) == len(pv), res
        assert mate_pv_ok(x["sfen"], pv), f"pv が詰みの手順になっていない: {pv}（{x['sfen']}）"
    lines = djp.job(99, f"mate side=opponent nodes=300000 position sfen {TSUMERO}")
    res = kv(of_job(lines, 99, "result")[0])
    assert res.get("mate") == "yes" and res.get("pv", "").split(",")[0] == "G*5h", f"side=opponent の詰めろを見つけない: {res}"
    lines = djp.job(100, "mate side=self nodes=50000 position startpos")
    res = kv(of_job(lines, 100, "result")[0])
    assert res.get("mate") in ("no", "unknown"), f"初期局面で詰みがあると言った: {res}"


def test_eval_job(djp):
    djp.hello()
    p0, p1 = "startpos moves 7g7f 3c3d", f"sfen {BLUNDER}"
    lines = djp.job(110, f"eval positions {p0} | {p1}")
    ev = sorted((int(kv(l)["index"]), kv(l)) for l in of_job(lines, 110, "evalres"))
    assert [i for i, _ in ev] == [0, 1], f"evalres の index が 0, 1 でない: {lines[-4:]}"
    for (i, d), p in zip(ev, (p0, p1)):
        djp.send(f"position {p}")
        djp.send("eval")
        line = djp.read_until(lambda l: l.startswith("eval raw"), timeout=10)[-1]
        assert int(d["cp"]) == int(line.split()[4]), f"eval のジョブの cp（{d['cp']}）が USI の eval（{line}）と違う"
        want = 1.0 / (1.0 + math.exp(-int(d["cp"]) / djp.eval_scale))
        assert abs(float(d["wr"]) - want) <= 0.01, d
    lines = djp.job(111, f"eval qsearch=1 positions {p1}")
    assert of_job(lines, 111, "evalres"), "qsearch=1 の eval のジョブが evalres を返さない"


# ---------------------------------------------------------------- 決定的モード・USI との共存

def strip(line):
    return " ".join(t for t in line.split() if not t.split("=")[0] in ("time", "nps", "hashfull"))


def test_deterministic_repeat(djp):
    djp.hello()
    djp.send("dajo set deterministic=1")
    djp.read_until(lambda l: l.startswith("dajo set"), timeout=30)
    runs = []
    for jid in (121, 122):
        djp.send("dajo newgame")
        lines = djp.job(jid, "search nodes=100000 multipv=2 position startpos moves 7g7f 3c3d 2g2f")
        runs.append([strip(l.replace(f" {jid} ", " N ", 1)) for l in lines
                     if l.split()[1:2] in (["move"], ["result"]) and l.split()[2] == str(jid)])
    assert runs[0] == runs[1], "決定的モードで、同じ search のジョブの結果が違う"


def test_usi_after_dajo(djp):
    djp.hello()
    djp.job(131, "search nodes=50000 position startpos")
    djp.send("position startpos moves 7g7f")
    djp.send("go nodes 30000")
    lines = djp.read_until(lambda l: l.startswith("bestmove"), timeout=60)
    assert not [l for l in lines if l.startswith("dajo")], "USI の go の途中で dajo の行を出した"
    b = board_of("startpos moves 7g7f")
    assert legal(b, lines[-1].split()[1]), lines[-1]
