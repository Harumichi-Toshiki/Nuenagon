// やねうら王の NNUE 形式（halfkp_256x2-32-32）の評価関数（設計書 §E10）。
// 特徴量・推論の定義は tests/halfkp_ref.py の冒頭のコメントのとおり。RayPair とは別の評価関数で、
// 探索は Game を通して使う（差分更新も Game の push / pop の中で行う）。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "board/shogi_utils.h"

namespace halfkp {

constexpr uint32_t kMagic = 0x7AF32F16;     // ファイルの先頭4バイト（版）。これなら HalfKP として読む
constexpr uint32_t kFileHash = 0x3E5AA6EE;  // ファイル全体のハッシュ（ヘッダ）
constexpr uint32_t kFtHash = 0x5D69D7B8;    // 特徴変換のハッシュ
constexpr uint32_t kNetHash = 0x63337156;   // ネットワークのハッシュ
constexpr int kW = 256;                     // 特徴変換の幅（視点ごと）
constexpr int kFeatures = 125388;           // 81 × 1548
constexpr int kL1 = 32, kL2 = 32;           // 隠れ層の幅
constexpr int kIn = 2 * kW;                 // 隠れ層1の入力（512）

// 読み込んだ重み（量子化済み。ファイルの値そのもの）
struct Network {
    std::string arch;                  // 構造の文字列（中身は検査しない）
    int16_t ft_bias[kW];
    std::vector<int16_t> ft_weight;    // [kFeatures][kW]
    int32_t b1[kL1];
    int8_t w1[kL1][kIn];
    int32_t b2[kL2];
    int8_t w2[kL2][kL1];
    int32_t bo;
    int8_t wo[kL2];
};

// 蓄積値。v[p] は視点 p（0 = 先手、1 = 後手）の kW 個（バイアス込み、int16）
struct Accumulator {
    int16_t v[2][kW];
};

// ファイルを読み、検査する。失敗なら false と理由を err に入れる
bool load_network(const std::string& path, Network& net, std::string& err);

// 指し手の差分更新に要る情報。指す前の盤から取る
struct MoveInfo {
    int from = -1;      // 移動元（駒打ちは -1）
    int to = 0;         // 移動先
    int piece = 0;      // 指す前の移動元の駒（盤の値。駒打ちは 0）
    int captured = 0;   // 取られた駒（盤の値。無ければ 0）
    int mover = 0;      // 指した側
    int drop_type = 0;  // 打った駒種（1..7。駒打ちのときだけ）
};
MoveInfo make_move_info(const Board& before, const Move& m);

// 全計算（両視点）
void full_accumulator(const Network& net, const Board& board, Accumulator& acc);

// 1手ぶんの差分更新。after は指した後の盤、prev は指す前の蓄積値、next は prev のコピーを入れておく
// （玉が動いた視点は全計算で作り直す。相手の視点は、玉が特徴に入らないので差分だけ）
void update_accumulator(const Network& net, const Board& after, const MoveInfo& mi,
                        const Accumulator& prev, Accumulator& next);

// 出力層の raw（手番 stm から見た、出力層の int32）
int32_t forward_raw(const Network& net, const Accumulator& acc, int stm);

}  // namespace halfkp
