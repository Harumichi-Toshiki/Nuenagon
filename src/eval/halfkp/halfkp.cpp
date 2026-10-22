// HalfKP（やねうら王の halfkp_256x2-32-32）の読み込み・全計算・差分更新・推論（設計書 §E10）。
// 定義は tests/halfkp_ref.py（やねうら王の値と一致することを確かめてある）。
#include "eval/halfkp/halfkp.h"

#include <cstring>
#include <fstream>
#include <iterator>

#include "eval/simd.h"
#if NUENAGON_SIMD_AVX2
#include <immintrin.h>
#endif

namespace halfkp {
namespace {

constexpr int kFeatureStride = 1548;  // 駒の番号の数（fe_end）。特徴の番号は 玉のマス × 1548 + 駒の番号
constexpr int kBoxPieces = 38;        // 玉以外の駒の総数（盤・持ち駒・駒箱）

// 駒の種類（盤の値 & 15）→ BonaPiece の k（歩0 香1 桂2 銀3 金4 角5 馬6 飛7 龍8。成駒は金と同じ）。玉・空は -1
constexpr int kKindOf[16] = {-1, 0, 1, 2, 3, 4, 5, 7, -1, 4, 4, 4, 4, -1, 6, 8};

// 持ち駒の駒種（1..7 = 歩香桂銀金角飛）→ 基準の番号（視点の側, 相手の側）
constexpr int kHandBase[7][2] = {{1, 20}, {39, 44}, {49, 54}, {59, 64}, {69, 74}, {79, 82}, {85, 88}};

// 盤のマス（内部の番号。y × 9 + x、x = 0 が 9 筋）→ HalfKP のマス番号（筋の番号 × 9 ＋ 段の番号。1一 = 0、9九 = 80）
int halfkp_sq(int sq) {
    return (8 - sq % 9) * 9 + sq / 9;
}

// 盤上の駒の特徴（玉のマス ksq と駒のマス sq は HalfKP のマス番号）。駒が無い・玉のときは -1
int board_feature(int ksq, int p, int piece, int sq) {
    const int k = kKindOf[piece & 15];
    if (k < 0) return -1;
    const int color = (piece & 16) ? 1 : 0;
    const int vsq = (p == 0) ? sq : 80 - sq;
    const int bp = 90 + 162 * k + (color == p ? 0 : 81) + vsq;
    return ksq * kFeatureStride + bp;
}

// 持ち駒の特徴。i は 0 から（枚数の 1 枚目が 0）
int hand_feature(int ksq, int p, int color, int type, int i) {
    const int base = kHandBase[type - 1][color == p ? 0 : 1];
    return ksq * kFeatureStride + base + i;
}

// 視点 p の玉のマス（HalfKP のマス番号。後手の視点は盤を180度回す）
int king_square(const Board& b, int p) {
    return (p == 0) ? halfkp_sq(b.king_sq[0]) : 80 - halfkp_sq(b.king_sq[1]);
}

// 視点 p の特徴の番号をすべて out に書き、個数を返す（全計算。重複を含む）
int collect_features(const Board& b, int p, int* out) {
    const int ksq = king_square(b, p);
    int n = 0, count = 0;
    for (int sq = 0; sq < 81; sq++) {
        const int piece = b.pieces[sq];
        if (piece == 0 || (piece & 15) == KING) continue;
        out[n++] = board_feature(ksq, p, piece, halfkp_sq(sq));
        count++;
    }
    for (int color = 0; color < 2; color++) {
        for (int t = 1; t <= 7; t++) {
            for (int i = 0; i < b.hand[color][t]; i++) {
                out[n++] = hand_feature(ksq, p, color, t, i);
                count++;
            }
        }
    }
    // 盤にも持ち駒にも無い駒（駒箱の駒）は、駒の番号 0 として数える
    for (int i = count; i < kBoxPieces; i++) out[n++] = ksq * kFeatureStride;
    return n;
}

// 視点 p の蓄積値を盤から作る（バイアス＋全部の行の和を int16 に丸める）
void full_one(const Network& net, const Board& b, int p, int16_t* out) {
    int idx[64];
    const int n = collect_features(b, p, idx);
    int32_t sum[kW];
    for (int j = 0; j < kW; j++) sum[j] = net.ft_bias[j];
    for (int i = 0; i < n; i++) {
        const int16_t* row = &net.ft_weight[(size_t)idx[i] * kW];
        for (int j = 0; j < kW; j++) sum[j] += row[j];
    }
    for (int j = 0; j < kW; j++) out[j] = (int16_t)sum[j];
}

// 蓄積値 v（kW 個）に、minus の行を引き、plus の行を足す。int16 の足し引きは mod 2^16 なので順序によらず同じ
void apply_rows(int16_t* v, const Network& net, const int* minus, int nm, const int* plus, int np) {
#if NUENAGON_SIMD_AVX2
    // 蓄積値の 256 要素を ymm レジスタ 16 本に載せたまま、全部の行を足し引きし、最後に1回だけ書き戻す
    __m256i a[16];
    for (int k = 0; k < 16; k++) a[k] = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + 16 * k));
    for (int t = 0; t < nm; t++) {
        const __m256i* row = reinterpret_cast<const __m256i*>(&net.ft_weight[(size_t)minus[t] * kW]);
        for (int k = 0; k < 16; k++) a[k] = _mm256_sub_epi16(a[k], _mm256_loadu_si256(row + k));
    }
    for (int t = 0; t < np; t++) {
        const __m256i* row = reinterpret_cast<const __m256i*>(&net.ft_weight[(size_t)plus[t] * kW]);
        for (int k = 0; k < 16; k++) a[k] = _mm256_add_epi16(a[k], _mm256_loadu_si256(row + k));
    }
    for (int k = 0; k < 16; k++) _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + 16 * k), a[k]);
#else
    for (int t = 0; t < nm; t++) {
        const int16_t* row = &net.ft_weight[(size_t)minus[t] * kW];
        for (int j = 0; j < kW; j++) v[j] = (int16_t)(v[j] - row[j]);
    }
    for (int t = 0; t < np; t++) {
        const int16_t* row = &net.ft_weight[(size_t)plus[t] * kW];
        for (int j = 0; j < kW; j++) v[j] = (int16_t)(v[j] + row[j]);
    }
#endif
}

// x（uint8）と w（int8）の内積。n は 32 の倍数。積の和は int32（両版で同じ値）
int32_t dot_u8_s8(const uint8_t* x, const int8_t* w, int n) {
#if NUENAGON_SIMD_AVX2
    // maddubs の2つの積の和は |和| ≤ 2 × 127 × 128 = 32,512 で、int16 に収まる（飽和しない）
    __m256i sum = _mm256_setzero_si256();
    const __m256i ones = _mm256_set1_epi16(1);
    for (int k = 0; k < n; k += 32) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + k));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + k));
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_maddubs_epi16(a, b), ones));
    }
    int32_t t[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(t), sum);
    return t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7];
#else
    int32_t s = 0;
    for (int k = 0; k < n; k++) s += (int32_t)x[k] * (int32_t)w[k];
    return s;
#endif
}

uint8_t clamp127(int32_t v) { return (uint8_t)(v < 0 ? 0 : (v > 127 ? 127 : v)); }

uint32_t read_u32(const std::vector<uint8_t>& d, size_t at) {
    return (uint32_t)d[at] | ((uint32_t)d[at + 1] << 8) | ((uint32_t)d[at + 2] << 16) | ((uint32_t)d[at + 3] << 24);
}

}  // namespace

MoveInfo make_move_info(const Board& before, const Move& m) {
    MoveInfo mi;
    mi.to = m.to;
    mi.mover = before.turn;
    if (m.is_drop) {
        mi.from = -1;
        mi.drop_type = m.type;
    }
    else {
        mi.from = m.from;
        mi.piece = before.pieces[m.from];
        mi.captured = before.pieces[m.to];
    }
    return mi;
}

bool load_network(const std::string& path, Network& net, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "ファイルを開けない: " + path;
        return false;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12) {
        err = "短すぎる";
        return false;
    }
    if (read_u32(d, 0) != kMagic) {
        err = "版が違う";
        return false;
    }
    const uint32_t file_hash = read_u32(d, 4);
    const uint32_t arch_len = read_u32(d, 8);
    if (arch_len > (1u << 20) || 12 + (size_t)arch_len > d.size()) {
        err = "構造の文字列の長さが合わない";
        return false;
    }
    // ファイルの長さ（ちょうど最後まで読み切ること）
    const size_t ft_at = 12 + arch_len;                      // 特徴変換のハッシュ
    const size_t ft_bias_at = ft_at + 4;
    const size_t ft_w_at = ft_bias_at + kW * 2;
    const size_t net_at = ft_w_at + (size_t)kFeatures * kW * 2;  // ネットワークのハッシュ
    const size_t l1b_at = net_at + 4;
    const size_t l1w_at = l1b_at + kL1 * 4;
    const size_t l2b_at = l1w_at + kL1 * kIn;
    const size_t l2w_at = l2b_at + kL2 * 4;
    const size_t ob_at = l2w_at + kL2 * kL1;
    const size_t ow_at = ob_at + 4;
    const size_t end = ow_at + kL2;
    if (d.size() != end) {
        err = "ファイルの長さが合わない";
        return false;
    }
    if (file_hash != kFileHash) {
        err = "ファイル全体のハッシュが合わない";
        return false;
    }
    if (read_u32(d, ft_at) != kFtHash) {
        err = "特徴変換のハッシュが合わない";
        return false;
    }
    if (read_u32(d, net_at) != kNetHash) {
        err = "ネットワークのハッシュが合わない";
        return false;
    }

    net.arch.assign(reinterpret_cast<const char*>(&d[12]), arch_len);
    auto i16 = [&d](size_t at) { return (int16_t)(uint16_t)(d[at] | (d[at + 1] << 8)); };
    auto i32 = [&d](size_t at) { return (int32_t)read_u32(d, at); };
    for (int j = 0; j < kW; j++) net.ft_bias[j] = i16(ft_bias_at + 2 * j);
    net.ft_weight.resize((size_t)kFeatures * kW);
    for (size_t i = 0; i < net.ft_weight.size(); i++) net.ft_weight[i] = i16(ft_w_at + 2 * i);
    for (int j = 0; j < kL1; j++) net.b1[j] = i32(l1b_at + 4 * j);
    for (int i = 0; i < kL1 * kIn; i++) (&net.w1[0][0])[i] = (int8_t)d[l1w_at + i];
    for (int j = 0; j < kL2; j++) net.b2[j] = i32(l2b_at + 4 * j);
    for (int i = 0; i < kL2 * kL1; i++) (&net.w2[0][0])[i] = (int8_t)d[l2w_at + i];
    net.bo = i32(ob_at);
    for (int k = 0; k < kL2; k++) net.wo[k] = (int8_t)d[ow_at + k];
    return true;
}

void full_accumulator(const Network& net, const Board& board, Accumulator& acc) {
    for (int p = 0; p < 2; p++) full_one(net, board, p, acc.v[p]);
}

void update_accumulator(const Network& net, const Board& after, const MoveInfo& mi,
                        const Accumulator& prev, Accumulator& next) {
    (void)prev;  // next は prev のコピーとして渡されている。ここでは next に差分を足し引きする
    for (int p = 0; p < 2; p++) {
        // 玉が動いた視点は、玉のマスが変わるので全計算
        if (mi.from >= 0 && (mi.piece & 15) == KING && mi.mover == p) {
            full_one(net, after, p, next.v[p]);
            continue;
        }
        const int ksq = king_square(after, p);
        int minus[8], plus[8];
        int nm = 0, np = 0;
        if (mi.from >= 0) {
            minus[nm++] = board_feature(ksq, p, mi.piece, halfkp_sq(mi.from));  // 移動元の駒（玉なら -1 で無し）
            if (mi.captured) minus[nm++] = board_feature(ksq, p, mi.captured, halfkp_sq(mi.to));
        }
        plus[np++] = board_feature(ksq, p, after.pieces[mi.to], halfkp_sq(mi.to));  // 移動先の駒（成った駒）
        if (mi.from >= 0) {
            // 取った駒は、取った側の持ち駒の、今の枚数の 1 枚目（枚数 − 1）になる。玉は持ち駒に入らない
            int t = mi.captured & 15;
            if (t > 8) t -= 8;  // 成駒は成る前の駒種
            if (mi.captured && t != KING) plus[np++] = hand_feature(ksq, p, mi.mover, t, after.hand[mi.mover][t] - 1);
        }
        else {
            // 打った駒は、持ち駒の、打った後の枚数（打つ前の枚数の最後の1枚）を引く
            minus[nm++] = hand_feature(ksq, p, mi.mover, mi.drop_type, after.hand[mi.mover][mi.drop_type]);
        }
        // 無効な特徴（-1）を除く
        int m2 = 0, p2 = 0;
        for (int t = 0; t < nm; t++) if (minus[t] >= 0) minus[m2++] = minus[t];
        for (int t = 0; t < np; t++) if (plus[t] >= 0) plus[p2++] = plus[t];
        apply_rows(next.v[p], net, minus, m2, plus, p2);
    }
}

int32_t forward_raw(const Network& net, const Accumulator& acc, int stm) {
    // 入力：手番側を先に、相手側を後に。clamp(0, 127) で uint8
    alignas(32) uint8_t x[kIn];
    for (int j = 0; j < kW; j++) {
        x[j] = clamp127(acc.v[stm][j]);
        x[kW + j] = clamp127(acc.v[1 - stm][j]);
    }
    // 隠れ層1：int32 の積和 ＋ bias を算術右シフト6 → clamp(0, 127)
    alignas(32) uint8_t h1[kL1];
    for (int j = 0; j < kL1; j++) {
        const int32_t s = net.b1[j] + dot_u8_s8(x, net.w1[j], kIn);
        h1[j] = clamp127(s >> 6);
    }
    // 隠れ層2
    alignas(32) uint8_t h2[kL2];
    for (int j = 0; j < kL2; j++) {
        const int32_t s = net.b2[j] + dot_u8_s8(h1, net.w2[j], kL1);
        h2[j] = clamp127(s >> 6);
    }
    int32_t raw = net.bo;
    for (int k = 0; k < kL2; k++) raw += (int32_t)net.wo[k] * (int32_t)h2[k];
    return raw;
}

}  // namespace halfkp
