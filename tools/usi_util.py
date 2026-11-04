"""USI のエンジンを 1 つ動かす共通の部品（tools/ の道具から使う。どの USI のエンジンでも動く）。

  e = Engine(exe, [("EvalFile", "x.bin"), ("Threads", 1)])
  infos, bestmove_line = e.go("position startpos", "go nodes 100000")
  e.close()
"""
import queue
import subprocess
import threading


class Engine:
    def __init__(self, exe, opts=(), cwd=None):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1, cwd=cwd)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        self.send("usi")
        self.usi_lines = self.wait("usiok", timeout=120)
        for k, v in opts:
            self.send(f"setoption name {k} value {v}")
        self.send("isready")
        self.wait("readyok", timeout=600)

    def _reader(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\r\n"))
        self.q.put(None)

    def send(self, s):
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def wait(self, prefix, timeout=60):
        """prefix で始まる行が来るまで読み、それまでの行（最後の行を含む）を返す。"""
        out = []
        while True:
            line = self.q.get(timeout=timeout)
            if line is None:
                raise RuntimeError("エンジンが終了した")
            out.append(line)
            if line == prefix or line.startswith(prefix + " "):
                return out

    def go(self, position_cmd, go_cmd, timeout=900):
        self.send(position_cmd)
        self.send(go_cmd)
        lines = self.wait("bestmove", timeout=timeout)
        infos = [x for x in lines if x.startswith("info depth ")]
        return infos, lines[-1]

    def close(self):
        try:
            self.send("quit")
            self.p.wait(timeout=10)
        except Exception:  # noqa: BLE001
            self.p.kill()


def score_of(infos):
    """最後の info の点数を (種類, 値) で返す。無ければ None。"""
    if not infos:
        return None
    t = infos[-1].split()
    if "score" not in t:
        return None
    i = t.index("score")
    return t[i + 1], int(t[i + 2])


def normalize_info(line):
    """時間・速さに依る項目を外す（決定的な比較用）。"""
    import re
    line = re.sub(r" nps \d+", "", line)
    line = re.sub(r" time \d+", "", line)
    line = re.sub(r" hashfull \d+", "", line)
    return line
