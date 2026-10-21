# 変更禁止（PROTECTED）。Nuenagon の受け入れテスト共通部品。
# エンジンの実行ファイルを起動し、標準入出力で1行ずつやり取りする。
import os
import platform
import subprocess
import threading
import queue

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def engine_path():
    p = os.environ.get("NUENAGON_BIN")
    if p:
        return p
    cands = ["build/nuenagon", "build/Release/nuenagon.exe", "build/nuenagon.exe"]
    for c in cands:
        q = os.path.join(ROOT, c)
        if os.path.exists(q):
            return q
    raise FileNotFoundError("エンジンの実行ファイルが見つかりません。NUENAGON_BIN で指定するか build/nuenagon に置いてください")


class Engine:
    def __init__(self):
        self.p = subprocess.Popen([engine_path()], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1, cwd=ROOT)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\r\n"))
        self.q.put(None)

    def send(self, s):
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def expect(self, prefix, timeout=60):
        """prefix で始まる行が来るまで読み、その行を返す（それ以外の行は読み捨てる）。"""
        while True:
            try:
                line = self.q.get(timeout=timeout)
            except queue.Empty:
                raise TimeoutError(f"'{prefix}' で始まる応答が {timeout} 秒以内に来ませんでした")
            if line is None:
                raise RuntimeError("エンジンが終了しました")
            if line == prefix or line.startswith(prefix + " "):
                return line

    def ask(self, cmd, prefix, timeout=60):
        self.send(cmd)
        return self.expect(prefix, timeout)

    def close(self):
        try:
            self.send("quit")
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


@pytest.fixture(scope="module")
def eng():
    e = Engine()
    yield e
    e.close()
