// 特徴量 RayPair_v0 / v1 の計算（設計書 §F2、凍結区画）。盤面（Board）から直接計算し、SFEN は経由しない。
//
// 全計算は次の4つを合わせたもの。すべて視点座標で計算する（§F1.2）。
//   A1  ：駒位置            … append_piece_features
//   A2  ：持ち駒            … append_hand_features
//   B1  ：ライン隣接対      … append_line_b1（ライン1本ぶん。N3 の差分更新はこの単位で行う）
//   B2  ：桂の利き          … append_knight_features
// 特徴は重複を許す多重集合（§F2.0）。同じインデックスが複数回出ても取り除かないこと。
//
// ライン番号（視点座標）：0〜8 縦（x）、9〜17 横（y）、18〜34 斜め↘（x − y = −8..8）、35〜51 斜め↙（x + y = 0..16）。
#pragma once
#include <vector>
#include "board/shogi_utils.h"

namespace raypair {

constexpr int kLines = 52;                 // ライン番号 0..51
constexpr int kDims[2] = {15000, 115352};  // 版（0 = v0、1 = v1）ごとの次元数

// 視点座標に直した局面
struct ViewBoard {
    int pc[81];      // 駒番号 pc（0..27）。空マスは -1。マス番号は視点座標の sq = y*9 + x
    int hand[2][7];  // 持ち駒。[0] は視点側、[1] は相手側。添字は駒種 h（0 = 歩 … 6 = 飛）
};

// persp：0 = 先手視点、1 = 後手視点
ViewBoard make_view(const Board& board, int persp);

void append_piece_features(const ViewBoard& vb, std::vector<int>& out);  // A1
void append_hand_features(const ViewBoard& vb, std::vector<int>& out);   // A2

// A1・A2 の1個ぶんの特徴のインデックス（N3 の差分更新で使う）
int piece_feature(int pc, int sq);         // 駒番号 pc（0..27）が視点座標 sq にある
int hand_feature(int side, int h, int k);  // 持ち駒。side は 0 = 視点側、1 = 相手側。k は 0..（枚数−1）

// version：0 = v0、1 = v1（領域付き）。line は 0..51
void append_line_b1(const ViewBoard& vb, int version, int line, std::vector<int>& out);

void append_knight_features(const ViewBoard& vb, int version, std::vector<int>& out);  // B2

// 全計算（両視点ぶんは呼び出し側で2回呼ぶ）。out は作り直し、昇順に並べて返す。
void compute_features(const Board& board, int persp, int version, std::vector<int>& out);

// ライン番号の表。sq（視点座標）を通る4本のライン（axis 0,1,2,3 の順）
struct LinesOfTable {
    int line[81][4];
};
constexpr LinesOfTable make_lines_of() {
    LinesOfTable t{};
    for (int sq = 0; sq < 81; sq++) {
        int y = sq / 9, x = sq % 9;
        t.line[sq][0] = x;
        t.line[sq][1] = 9 + y;
        t.line[sq][2] = 26 + x - y;
        t.line[sq][3] = 35 + x + y;
    }
    return t;
}
inline constexpr LinesOfTable kLinesOf = make_lines_of();

// ライン番号から軸（0 縦、1 横、2 斜め↘、3 斜め↙）
int axis_of_line(int line);

// ライン上のマスを走査順（§F2.1 B1 の表の「手前 → 奥」）に out へ書き、マス数を返す。out は9要素以上
int line_squares(int line, int* out);

}  // namespace raypair
