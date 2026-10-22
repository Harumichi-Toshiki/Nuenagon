// 重みファイル（.shk）の読み込み・量子化（§F4、§E4）、蓄積値の全計算と差分更新（§E3）、推論（§E4）。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "eval/raypair/raypair.h"

#include "eval/simd.h"

namespace raypair {

constexpr int kW = 128;  // 第1層の幅 W（v0・v1 とも 128）

// 量子化済みの重み（§E4 の表）
struct Network {
    int version = 0;               // 0 = RayPair_v0Network、1 = RayPair_v1Network
    std::string feature_set;       // 特徴セット ID（例：RayPair_v0）
    std::string network_id;        // ネットワーク ID（例：RayPair_v0Network）
    int dims = 0;                  // 特徴量の次元数
    int L1 = 0, L2 = 0;
    float eval_scale = 0;          // ヘッダの eval_scale（f32）
    std::vector<int16_t> ft_weight;  // [dims][kW]
    int16_t ft_bias[kW];           // [kW]
    std::vector<int8_t> l1_weight;   // [L1][2kW]
    std::vector<int32_t> l1_bias;    // [L1]
    std::vector<int8_t> l2_weight;   // [L2][L1]
    std::vector<int32_t> l2_bias;    // [L2]
    std::vector<int8_t> out_weight;  // [L2]
    int32_t out_bias = 0;
};

// 第1層の蓄積値。v[p] は視点 p（0 = 先手、1 = 後手）の値（バイアス込み、int16、W 個）
struct Accumulator {
    int16_t v[2][kW];
};

// 重みファイルを読み、検査（§F4.3）して量子化する。失敗なら false と理由を err に入れる
bool load_network(const std::string& path, Network& net, std::string& err);

// 全計算（§E3.1）。両視点の蓄積値を、盤面から作り直す
void full_accumulator(const Network& net, const Board& board, Accumulator& acc);

// 1手ぶんの差分更新（§E3.2）。p 視点の蓄積値 acc[p] を、変わったもの d で更新する。post は指した後の視点の盤面
void update_accumulator(const Network& net, int p, const ViewBoard& post, const ViewDelta& d, Accumulator& acc);

// 最終出力 raw（§E4）。stm は手番（0 = 先手、1 = 後手）
int32_t forward_raw(const Network& net, const Accumulator& acc, int stm);

// 評価値 cp = round(raw × eval_scale / 8128)（§E4。0 から遠い方への四捨五入）
long long cp_of(int32_t raw, double eval_scale);

}  // namespace raypair
