# 変更禁止（PROTECTED）。N4 の強さのテストで使う、Python の簡単な対戦相手。
# 2手読み（αβ）＋駒の損得だけで指す。読み筋が同点のときは乱数で選ぶ（種で決まる）。
import random

import cshogi

# cshogi の駒種（1..14）の価値（成駒は元の駒＋少し）
VALUE = {1: 1, 2: 3, 3: 4, 4: 5, 5: 8, 6: 10, 7: 6, 8: 0, 9: 6, 10: 6, 11: 6, 12: 6, 13: 11, 14: 13}
HAND_VALUE = [1, 3, 4, 5, 6, 8, 10]  # 歩香桂銀金角飛（cshogi の pieces_in_hand の順）
MATE = 100000


def material(b):
    """手番側から見た駒の損得。"""
    s = 0
    for p in b.pieces:
        if p:
            v = VALUE[p & 15]
            s += v if p < 16 else -v
    hb, hw = b.pieces_in_hand
    s += sum(HAND_VALUE[i] * hb[i] for i in range(7)) - sum(HAND_VALUE[i] * hw[i] for i in range(7))
    return s if b.turn == cshogi.BLACK else -s


def negamax(b, depth, alpha, beta):
    moves = list(b.legal_moves)
    if not moves:
        return -MATE
    if depth == 0:
        return material(b)
    best = -MATE - 1
    for m in moves:
        b.push(m)
        v = -negamax(b, depth - 1, -beta, -alpha)
        b.pop()
        if v > best:
            best = v
        if v > alpha:
            alpha = v
        if alpha >= beta:
            break
    return best


class Baseline:
    def __init__(self, seed):
        self.rng = random.Random(seed)

    def choose(self, b):
        moves = list(b.legal_moves)
        self.rng.shuffle(moves)
        best, bv = None, -MATE - 2
        for m in moves:
            b.push(m)
            v = -negamax(b, 1, -MATE - 1, MATE + 1)
            b.pop()
            if v > bv:
                best, bv = m, v
        return cshogi.move_to_usi(best)
