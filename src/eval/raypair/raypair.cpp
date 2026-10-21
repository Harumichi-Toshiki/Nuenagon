// 特徴量 RayPair_v0 / v1 の計算（設計書 §F2）。定義は凍結区画で、ここでは変えない。
#include "raypair.h"
#include <algorithm>

namespace raypair {
namespace {

constexpr int kA1Base = 0;
constexpr int kA2Base = 2268;
constexpr int kB1Base = 2344;
constexpr int kB1Local = 12544;  // 28 * 448。1領域ぶんの B1 の幅（pc_lo*448 + pc_hi*16 + axis*4 + db）

// 盤面の駒種（Board の値 & 15）→ 駒番号 pc（視点側）。13 は欠番、14 は馬（pc 12）、15 は竜（pc 13）
constexpr int kPcOfType[16] = {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1, 12, 13};

// 持ち駒の駒種番号 h（0..6）のオフセット off[h]（§F1.4）
constexpr int kHandOff[7] = {0, 18, 22, 26, 30, 34, 36};

// 距離 d → 距離区分 db（§F2.1 B1）
int db_of(int d) {
    if (d == 1) return 0;
    if (d == 2) return 1;
    if (d <= 4) return 2;
    return 3;
}

}  // namespace

ViewBoard make_view(const Board& board, int persp) {
    ViewBoard vb;
    for (int sq = 0; sq < 81; sq++) vb.pc[sq] = -1;
    for (int sq = 0; sq < 81; sq++) {
        int piece = board.pieces[sq];
        if (piece == 0) continue;
        int type = piece & 15;
        int owner = (piece & 16) ? WHITE : BLACK;
        int vsq = (persp == BLACK) ? sq : 80 - sq;  // 後手視点は盤を180度回す（§F1.2）
        vb.pc[vsq] = kPcOfType[type] + (owner == persp ? 0 : 14);
    }
    for (int h = 0; h < 7; h++) {
        vb.hand[0][h] = board.hand[persp][h + 1];
        vb.hand[1][h] = board.hand[1 - persp][h + 1];
    }
    return vb;
}

namespace {

// 盤面の駒（Board::pieces の値）→ 視点 persp の駒番号。空は -1（make_view と同じ式）
int pc_of_piece(int piece, int persp) {
    if (piece == 0) return -1;
    const int owner = (piece & 16) ? WHITE : BLACK;
    return kPcOfType[piece & 15] + (owner == persp ? 0 : 14);
}

}  // namespace

void set_view_squares(ViewBoard& vb, int persp, const Board& board, const int* sqs, int n, ViewDelta* d) {
    if (d) {
        d->n = 0;
        for (int h = 0; h < 7; h++) {
            d->pre_hand[0][h] = vb.hand[0][h];
            d->pre_hand[1][h] = vb.hand[1][h];
        }
    }
    for (int i = 0; i < n; i++) {
        if (sqs[i] < 0) continue;
        const int vsq = (persp == BLACK) ? sqs[i] : 80 - sqs[i];  // make_view と同じ向き
        if (d) {
            d->sq[d->n] = vsq;
            d->pre_pc[d->n] = vb.pc[vsq];
            d->n++;
        }
        vb.pc[vsq] = pc_of_piece(board.pieces[sqs[i]], persp);
    }
    for (int h = 0; h < 7; h++) {
        vb.hand[0][h] = board.hand[persp][h + 1];
        vb.hand[1][h] = board.hand[1 - persp][h + 1];
    }
}

int b1_feature(int version, int axis, int lo_sq, int pc_lo, int hi_sq, int pc_hi) {
    const int ya = lo_sq / 9, xa = lo_sq % 9;
    const int yb = hi_sq / 9, xb = hi_sq % 9;
    const int d = (axis == 1) ? xb - xa : yb - ya;
    const int local = pc_lo * 448 + pc_hi * 16 + axis * 4 + db_of(d);
    const int region = (version == 0) ? 0 : (ya / 3) * 3 + (xa / 3);
    return kB1Base + region * kB1Local + local;
}

int knight_feature(int version, int s, int pc_target, int lr) {
    const int base = kB1Base + (version == 0 ? 1 : 9) * kB1Local;
    return base + s * 56 + pc_target * 2 + lr;
}

void append_piece_features(const ViewBoard& vb, std::vector<int>& out) {
    for (int sq = 0; sq < 81; sq++) {
        if (vb.pc[sq] >= 0) out.push_back(kA1Base + vb.pc[sq] * 81 + sq);
    }
}

int piece_feature(int pc, int sq) {
    return kA1Base + pc * 81 + sq;
}

int hand_feature(int side, int h, int k) {
    return kA2Base + side * 38 + kHandOff[h] + k;
}

void append_hand_features(const ViewBoard& vb, std::vector<int>& out) {
    for (int side = 0; side < 2; side++) {
        for (int h = 0; h < 7; h++) {
            for (int k = 0; k < vb.hand[side][h]; k++) {
                out.push_back(kA2Base + side * 38 + kHandOff[h] + k);
            }
        }
    }
}

void append_line_b1(const ViewBoard& vb, int version, int line, std::vector<int>& out) {
    int sq[9];
    int n = line_squares(line, sq);
    int axis = axis_of_line(line);
    int prev = -1;  // 直前の駒があるマス（走査で先に現れる方が lo）
    for (int i = 0; i < n; i++) {
        int cur = sq[i];
        if (vb.pc[cur] < 0) continue;
        if (prev >= 0) {
            int ya = prev / 9, xa = prev % 9;
            int yb = cur / 9, xb = cur % 9;
            int d = (axis == 1) ? xb - xa : yb - ya;
            int local = vb.pc[prev] * 448 + vb.pc[cur] * 16 + axis * 4 + db_of(d);
            int region = (version == 0) ? 0 : (ya / 3) * 3 + (xa / 3);  // v1 は lo のマスの領域
            out.push_back(kB1Base + region * kB1Local + local);
        }
        prev = cur;
    }
}

void append_knight_features(const ViewBoard& vb, int version, std::vector<int>& out) {
    int base = kB1Base + (version == 0 ? 1 : 9) * kB1Local;
    for (int sq = 0; sq < 81; sq++) {
        int pc = vb.pc[sq];
        if (pc != 2 && pc != 16) continue;  // 成っていない桂。2 は視点側、16 は相手側
        int s = (pc == 2) ? 0 : 1;
        int y = sq / 9, x = sq % 9;
        int ty = (s == 0) ? y - 2 : y + 2;
        for (int lr = 0; lr < 2; lr++) {
            int tx = (lr == 0) ? x - 1 : x + 1;
            if (ty < 0 || ty > 8 || tx < 0 || tx > 8) continue;
            int target = vb.pc[ty * 9 + tx];
            if (target >= 0) out.push_back(base + s * 56 + target * 2 + lr);
        }
    }
}

void compute_features(const Board& board, int persp, int version, std::vector<int>& out) {
    ViewBoard vb = make_view(board, persp);
    out.clear();
    append_piece_features(vb, out);
    append_hand_features(vb, out);
    for (int line = 0; line < kLines; line++) append_line_b1(vb, version, line, out);
    append_knight_features(vb, version, out);
    std::sort(out.begin(), out.end());
}

int axis_of_line(int line) {
    if (line < 9) return 0;
    if (line < 18) return 1;
    if (line < 35) return 2;
    return 3;
}

int line_squares(int line, int* out) {
    int n = 0;
    switch (axis_of_line(line)) {
    case 0:  // 縦：x 固定、y 増加
        for (int y = 0; y < 9; y++) out[n++] = y * 9 + line;
        break;
    case 1:  // 横：y 固定、x 増加
        for (int x = 0; x < 9; x++) out[n++] = (line - 9) * 9 + x;
        break;
    case 2:  // 斜め↘：c = x − y 固定、y 増加
        for (int y = 0; y < 9; y++) {
            int x = y + (line - 26);
            if (x >= 0 && x <= 8) out[n++] = y * 9 + x;
        }
        break;
    default:  // 斜め↙：s = x + y 固定、y 増加（x は減る）
        for (int y = 0; y < 9; y++) {
            int x = (line - 35) - y;
            if (x >= 0 && x <= 8) out[n++] = y * 9 + x;
        }
        break;
    }
    return n;
}

LineTable make_line_table() {
    LineTable t{};
    for (int line = 0; line < kLines; line++) {
        t.axis[line] = axis_of_line(line);
        t.n[line] = line_squares(line, t.sq[line]);
    }
    return t;
}

}  // namespace raypair
