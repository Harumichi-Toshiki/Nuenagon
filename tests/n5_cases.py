# 変更禁止（PROTECTED）。N5 の受け入れテストの共通部品：局面集、重みファイルの作り方、探索結果の取り出し方。
# 正解データ（golden/n5_golden.json）は、N4.1（コミット ae08a26）に「決定的モードではルートの詰み探索を
# 時間で止めない」（設計書 v1.4 §E5.9）だけを加えたエンジンで、この部品を使って作った。
import hashlib
import json
import os
import queue
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "reference"))

GOLDEN = os.path.join(HERE, "golden", "n5_golden.json")
MATERIAL = os.path.join(HERE, "golden", "material_v0.shk")
N1 = json.load(open(os.path.join(HERE, "golden", "n1_golden.json"), encoding="utf-8"))

# N4.1 の強さのテストで負けた対局から取った、持ち駒の多い中盤
MIDDLEGAME = [
    "ln3+B2l/2s2k3/rpp2pnsp/6pp1/p8/P5P2/1PP2KNPP/6S+bL/LNSGG4 w 2GPr4p 44",
    "ln6l/2s6/r4knsp/1pp2ppp1/P8/6P2/1PPG1KNPP/7+bL/LNSGGS3 w Grb6p 56",
    "ln6l/2s6/r5ns1/5k1pp/PG3pP2/9/1PPG1KNPP/7+bL/LNSGGS3 w 2Prb7p 68",
    "ln6+N/2s6/r6s1/9/PG4k2/4G1P1p/1PP2K1+pP/7+bL/LNSGGS3 w NLPrb10p 82",
]
OPENINGS = [
    "startpos",
    "startpos moves 7g7f 3c3d 2g2f 8c8d",
    "startpos moves 7g7f 8c8d 2g2f 3c3d 2f2e 2b2c",
    "startpos moves 2g2f 8c8d 7g7f 3c3d 6i7h 4a3b",
]
# N1 の乱数の対局から、手数がばらけるように選んだ局面（でたらめな手の対局なので、持ち駒が多く駒がぶつかりやすい）
RANDOM_IDX = [5, 30, 55, 80, 105, 130, 155, 180, 205, 230, 255, 280, 305, 330, 355, 380]


# 速さのテストの局面（序盤4・持ち駒の多い中盤4・乱数の対局4）
SPEED_POSITIONS = OPENINGS + MIDDLEGAME + ["startpos moves " + " ".join(N1["random_positions"][i]["moves"])
                                           for i in (55, 130, 205, 330)]


def pos_cmd(p):
    if p.startswith("startpos"):
        return "position " + p
    return "position sfen " + p


def mate_positions():
    """N1 の詰将棋から、5手詰めと7手詰めを2つずつ（ルートの詰み探索の決定性の確認）。"""
    out = []
    for length in (5, 7):
        xs = [x for x in N1["mate"] if x["length"] == length][:2]
        out += [x["sfen"] for x in xs]
    return out


def cases():
    """(名前, 重みの種類, 局面, MultiPV, ノード数) の一覧。"""
    cs = []
    for p in OPENINGS:
        cs.append(("material/opening", "material", p, 1, 150000))
    for p in MIDDLEGAME:
        cs.append(("material/middlegame", "material", p, 1, 150000))
    for i in RANDOM_IDX:
        cs.append((f"material/random{i}", "material", "startpos moves " + " ".join(N1["random_positions"][i]["moves"]), 1, 100000))
    for p in mate_positions():
        cs.append(("material/mate", "material", p, 1, 50000))
    for p in (OPENINGS[2], MIDDLEGAME[1], "startpos moves " + " ".join(N1["random_positions"][130]["moves"])):
        cs.append(("material/multipv3", "material", p, 3, 80000))
    for p in (OPENINGS[0], OPENINGS[3], MIDDLEGAME[0], MIDDLEGAME[2]) + tuple(
            "startpos moves " + " ".join(N1["random_positions"][i]["moves"]) for i in (55, 205, 330, 380)):
        cs.append(("v1_random", "v1_random", p, 1, 40000))
        cs.append(("v0_extreme", "v0_extreme", p, 1, 100000))
    return cs


# ---------------------------------------------------------------- 重みファイル

SHK_SHA256 = {}  # 正解データを作ったときの、生成した重みファイルの SHA-256（golden に記録し、テストで照合する）


def make_shk(kind, path):
    """kind：'material'（tests/golden/material_v0.shk）、'v1_random'・'v0_extreme'（N3 のテストと同じ作り方で生成）。"""
    if kind == "material":
        return MATERIAL
    import m4_ref
    from test_n3_acceptance import header, make_weights
    v, wkind, seed = {"v1_random": (1, "random", 501), "v0_extreme": (0, "extreme", 502)}[kind]
    t = make_weights(v, wkind, seed=seed)
    data = m4_ref.build_shk(header(v), t)
    with open(path, "wb") as f:
        f.write(data)
    return path


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


# ---------------------------------------------------------------- エンジン

class Engine:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  text=True, bufsize=1, cwd=ROOT)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\r\n"))
        self.q.put(None)

    def send(self, s):
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def lines_until(self, prefix, timeout=120):
        out = []
        end = time.time() + timeout
        while True:
            rest = end - time.time()
            if rest <= 0:
                raise TimeoutError(f"'{prefix}' が {timeout} 秒以内に来ない")
            try:
                line = self.q.get(timeout=rest)
            except queue.Empty:
                continue
            if line is None:
                raise RuntimeError("エンジンが終了した")
            out.append(line)
            if line == prefix or line.startswith(prefix + " "):
                return out

    def close(self):
        try:
            self.send("quit")
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def setup(eng, shk, deterministic=True, multipv=1, threads=1):
    eng.send("usi")
    eng.lines_until("usiok")
    for k, v in (("EvalFile", shk), ("Threads", threads), ("USI_Hash", 64), ("MultiPV", multipv),
                 ("Deterministic", "true" if deterministic else "false"), ("NetworkDelay", 0),
                 ("USI_Ponder", "false"), ("DebugCheckAccumulator", "false")):
        eng.send(f"setoption name {k} value {v}")
    eng.send("isready")
    eng.lines_until("readyok", timeout=300)


def normalize_info(line):
    """info の行から、時間に左右されない部分（depth・seldepth・multipv・score・nodes・pv）だけを取り出す。"""
    t = line.split()
    d = {}
    i = 1
    while i < len(t):
        k = t[i]
        if k == "pv":
            d["pv"] = " ".join(t[i + 1:])
            break
        if k == "score":
            d["score"] = t[i + 1] + " " + t[i + 2]
            i += 3
            continue
        if k in ("depth", "seldepth", "multipv", "nodes"):
            d[k] = t[i + 1]
        i += 2
    return d


def run_case(eng, pos, multipv, nodes, timeout=300):
    """usinewgame → position → go nodes。info（depth を含む行）の正規化した一覧と bestmove の行を返す。"""
    eng.send(f"setoption name MultiPV value {multipv}")
    eng.send("usinewgame")
    eng.send(pos_cmd(pos))
    t0 = time.time()
    eng.send(f"go nodes {nodes}")
    lines = eng.lines_until("bestmove", timeout=timeout)
    el = time.time() - t0
    infos = [normalize_info(x) for x in lines if x.startswith("info depth ")]
    return {"infos": infos, "bestmove": lines[-1]}, el
