// やねうら王の古典的な NNUE（nn.bin。<特徴>_<W>x2-<L1>-<L2>）の評価関数（設計書 §E10、N7）。
// 特徴は HalfKP・HalfKA1・HalfKA2・HalfKA_hm1・HalfKA_hm2、幅 W・L1・L2 は実行時に決める。
// 特徴・推論・ファイルの並びの定義は tests/nnue_ref.py・tests/nnue_nnbin.py（やねうら王と一致することを確かめてある）。
// 構造は構造の文字列を読まず、特徴変換のハッシュ・ネットワークのハッシュ・ファイルの長さから決める。
// RayPair とは別の評価関数。探索は Game を通して使う（差分更新も Game の push / pop の中で行う）。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "board/shogi_utils.h"

namespace nnue {

constexpr uint32_t kMagic = 0x7AF32F16;  // ファイルの先頭4バイト（版）。これなら nn.bin として読む
constexpr int kMaxW = 8192;              // 特徴変換の幅の上限（Shikibu の範囲）
constexpr int kMaxL = 1024;              // 隠れ層の幅の上限

enum class Feature { HalfKP, HalfKA1, HalfKA2, HalfKAhm1, HalfKAhm2 };

// 読み込んだ重み（量子化済み。ファイルの値そのもの）
struct Network {
    Feature feature = Feature::HalfKP;
    std::string feature_name;   // info 用（"HalfKP"・"HalfKA_hm2" など）
    std::string network_name;   // info 用（"halfkp_256x2-32-32" など。小文字）
    int W = 0, L1 = 0, L2 = 0;  // 特徴変換の幅（視点ごと）・隠れ層の幅
    int feat_dim = 0;           // 特徴の次元
    int in_pad = 0;             // pad32(2W)。第1層の重みの1行の長さ
    int l1_pad = 0;             // pad32(L1)。第2層の重みの1行の長さ
    int l2_pad = 0;             // pad32(L2)。出力層の重みの長さ
    std::vector<int16_t> ft_bias;    // [W]
    std::vector<int16_t> ft_weight;  // [feat_dim][W]
    std::vector<int32_t> b1;         // [L1]
    std::vector<int8_t> w1;          // [L1][in_pad]
    std::vector<int32_t> b2;         // [L2]
    std::vector<int8_t> w2;          // [L2][l1_pad]
    int32_t bo = 0;
    std::vector<int8_t> wo;          // [l2_pad]

    // 蓄積値 1 つの長さ（int16 の個数）。視点 p の蓄積値は p × W から W 個
    int acc_len() const { return 2 * W; }
};

// Finny テーブル：視点 p と、玉の元のマス key ごとに、最後に作った蓄積値と、そのときの盤・持ち駒を覚える。
// 玉を動かした視点の作り直しで、覚えた盤との差分だけを足し引きする。盤の状態だけで決まるので、履歴によらず結果は同じ。
struct Finny {
    struct Slot {
        bool valid = false;
        uint8_t piece[81] = {};
        uint8_t hand[2][8] = {};
        std::vector<int16_t> acc;  // 視点 p の蓄積値（W 個）
    };
    Slot slots[2][81];
    void reset() {
        for (auto& row : slots) for (auto& s : row) s.valid = false;
    }
};

// ファイルを読み、構造を決め、検査する。失敗なら false と理由を err に入れる
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

// 全計算（両視点）。acc は acc_len() 個の int16（視点 p は p × W から）
void full_accumulator(const Network& net, const Board& board, int16_t* acc);

// 1手ぶんの差分更新。after は指した後の盤、prev は指す前の蓄積値、next は書き込み先（acc_len() 個）。
// 自分の玉が動いた視点は全計算で作り直す。相手の視点は、玉が特徴に入るときは玉の移動も差分で扱う
// fc が null でなければ、玉を動かした視点は Finny テーブルで作る（結果は同じ）
void update_accumulator(const Network& net, const Board& after, const MoveInfo& mi,
                        const int16_t* prev, int16_t* next, Finny* fc = nullptr);

// 出力層の raw（手番 stm から見た、出力層の int32）
int32_t forward_raw(const Network& net, const int16_t* acc, int stm);

}  // namespace nnue
