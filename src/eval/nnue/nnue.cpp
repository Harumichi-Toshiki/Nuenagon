// やねうら王の古典的な NNUE（nn.bin）の読み込み・全計算・差分更新・推論（設計書 §E10、N7）。
// 定義は tests/nnue_ref.py・tests/nnue_nnbin.py。特徴の番号は HalfKP の 玉のマス × 1548 ＋ 駒 を、
// HalfKA の系統では玉のマス × 1629（または 1710）＋ 駒 に広げ、hm は玉が 6〜9 筋のとき左右を反転する。
#include "platform/file_util.h"
#include "eval/nnue/nnue.h"

#include <cstring>

#include "eval/simd.h"
#if NUENAGON_SIMD_AVX2
#include <immintrin.h>
#endif

namespace nnue {
namespace {

constexpr int kFKing = 1548;   // 玉（自分）の番号の始まり（HalfKA）
constexpr int kEKing = 1629;   // 玉（相手）の番号の始まり（HalfKA）
constexpr int kHandEnd = 90;   // 持ち駒の番号の終わり（盤上の駒の始まり）
constexpr int kPlaneHalfKP = 1548;
constexpr int kPlaneHalfKA1 = 1710;
constexpr int kPlaneHalfKA2 = 1629;  // 相手の玉を自分の玉と同じ番号にしたもの

// 駒の種類（盤の値 & 15）→ BonaPiece の k（歩0 香1 桂2 銀3 金4 角5 馬6 飛7 龍8。成駒は金と同じ）。玉・空は -1
constexpr int kKindOf[16] = {-1, 0, 1, 2, 3, 4, 5, 7, -1, 4, 4, 4, 4, -1, 6, 8};

// 持ち駒の駒種（1..7 = 歩香桂銀金角飛）→ 基準の番号（視点の側, 相手の側）
constexpr int kHandBase[7][2] = {{1, 20}, {39, 44}, {49, 54}, {59, 64}, {69, 74}, {79, 82}, {85, 88}};

struct FeatureDef {
    const char* name;   // やねうら王の名前（info 用）
    const char* lower;  // 構造の名前の接頭辞（小文字）
    uint32_t hash;      // 特徴のハッシュ（特徴変換のハッシュ = これ ^ 2W）
    bool king;          // 玉を特徴に入れるか（HalfKA の系統）
    bool hm;            // 左右反転するか
    bool merge;         // 相手の玉を自分の玉と同じ番号にするか（halfka2 系）
};
constexpr FeatureDef kDefs[5] = {
    {"HalfKP", "halfkp", 0x5D69D5B8u, false, false, false},
    {"HalfKA1", "halfka1", 0x5F134CB8u, true, false, false},
    {"HalfKA2", "halfka2", 0x5F234CB8u, true, false, true},
    {"HalfKA_hm1", "halfkahm1", 0x7F134CB8u, true, true, false},
    {"HalfKA_hm2", "halfkahm2", 0x7F234CB8u, true, true, true},
};

int plane_of(const FeatureDef& f) { return !f.king ? kPlaneHalfKP : (f.merge ? kPlaneHalfKA2 : kPlaneHalfKA1); }
int buckets_of(const FeatureDef& f) { return f.hm ? 45 : 81; }  // 玉のマスの数（hm は 0〜44）

// マスの番号：内部の盤（sq = 9 × 段 ＋ (9 − 筋)）から、筋の番号 × 9 ＋ 段の番号（1一 = 0、9九 = 80）へ
int halfkp_sq(int sq) { return (8 - sq % 9) * 9 + sq / 9; }
// 左右の反転（筋の番号を 8 − 筋に）
int mir(int v) { return (8 - v / 9) * 9 + v % 9; }

uint32_t affine_hash(uint32_t prev, uint32_t out) {
    return (0xCC03DAE4u + out) ^ (prev >> 1) ^ (prev << 31);
}
uint32_t relu_hash(uint32_t prev) { return 0x538D24C7u + prev; }

// ネットワークのハッシュ（tests/nnue_nnbin.py の hashes() と同じ式）
uint32_t net_hash(int W, int L1, int L2) {
    uint32_t h = 0xEC42E90Du ^ (uint32_t)(2 * W);
    h = relu_hash(affine_hash(h, (uint32_t)L1));
    h = relu_hash(affine_hash(h, (uint32_t)L2));
    return affine_hash(h, 1);
}

int pad32(int n) { return (n + 31) / 32 * 32; }

// ファイルの長さ（tests/nnue_nnbin.py の file_size() と同じ式）
size_t file_size_of(size_t arch_len, const FeatureDef& f, int W, int L1, int L2) {
    const size_t dim = (size_t)plane_of(f) * buckets_of(f);
    return 12 + arch_len + 4 + 2 * (size_t)W + 2 * (size_t)W * dim + 4
         + 4 * (size_t)L1 + (size_t)L1 * pad32(2 * W) + 4 * (size_t)L2 + (size_t)L2 * pad32(L1)
         + 4 + (size_t)pad32(L2);
}

// 視点 p の見え方：玉のマス（hm の反転後）× 特徴の次元の 1 玉あたり、反転の有無
struct View {
    int p;        // 視点（0 = 先手、1 = 後手）
    int base;     // 玉のマス（反転後）× plane
    int plane;    // 1 玉あたりの駒の番号の数
    bool king;    // 玉を特徴に入れるか
    bool merge;   // 相手の玉を自分の玉と同じ番号にするか
    bool mirror;  // 左右を反転するか
};

View make_view(const Network& net, const Board& b, int p) {
    const FeatureDef& f = kDefs[(int)net.feature];
    View v{};
    v.p = p;
    v.plane = plane_of(f);
    v.king = f.king;
    v.merge = f.merge;
    int ksq = (p == 0) ? halfkp_sq(b.king_sq[0]) : 80 - halfkp_sq(b.king_sq[1]);
    v.mirror = f.hm && ksq / 9 >= 5;
    if (v.mirror) ksq = mir(ksq);
    v.base = ksq * v.plane;
    return v;
}

// 盤上の駒の特徴（hsq は HalfKP のマス番号。駒が無い・対象外の玉は -1）
int board_index(const View& v, int piece, int hsq) {
    const int t = piece & 15;
    if (t == 0) return -1;
    const bool mine = (((piece & 16) ? 1 : 0) == v.p);
    int vsq = (v.p == 0) ? hsq : 80 - hsq;
    if (v.mirror) vsq = mir(vsq);
    int bp;
    if (t == KING) {
        if (!v.king) return -1;
        bp = (mine || v.merge ? kFKing : kEKing) + vsq;
    }
    else {
        bp = 90 + 162 * kKindOf[t] + (mine ? 0 : 81) + vsq;
    }
    return v.base + bp;
}

// 持ち駒の特徴（i は枚数の 1 枚目が 0）。反転しない
int hand_index(const View& v, int color, int type, int i) {
    const int base = kHandBase[type - 1][color == v.p ? 0 : 1];
    return v.base + base + i;
}

// 視点の特徴の番号をすべて out に書き、個数を返す（全計算。重複を含む）
int collect(const View& v, const Board& b, int* out) {
    const int total = v.king ? 40 : 38;  // 玉を含めた駒の数（盤・持ち駒・駒箱）
    int n = 0, count = 0;
    for (int sq = 0; sq < 81; sq++) {
        const int piece = b.pieces[sq];
        if (piece == 0) continue;
        if (!v.king && (piece & 15) == KING) continue;
        out[n++] = board_index(v, piece, halfkp_sq(sq));
        count++;
    }
    for (int color = 0; color < 2; color++) {
        for (int t = 1; t <= 7; t++) {
            for (int i = 0; i < b.hand[color][t]; i++) {
                out[n++] = hand_index(v, color, t, i);
                count++;
            }
        }
    }
    // 盤にも持ち駒にも無い駒（駒箱の駒）は、駒の番号 0 として数える
    for (int i = count; i < total; i++) out[n++] = v.base;
    return n;
}

// 蓄積値 v（W 個）に、minus の行を引き、plus の行を足す。int16 の足し引きは mod 2^16 なので順序によらず同じ
void apply_rows(int16_t* out, const Network& net, const int* minus, int nm, const int* plus, int np) {
    const int W = net.W;
    const int16_t* mr[8];
    const int16_t* pr[8];
    for (int t = 0; t < nm; t++) mr[t] = &net.ft_weight[(size_t)minus[t] * W];
    for (int t = 0; t < np; t++) pr[t] = &net.ft_weight[(size_t)plus[t] * W];
#if NUENAGON_SIMD_AVX2
    // 16 要素（ymm 1 本）ずつ、全部の行を足し引きして、そのまま書き戻す（W は 32 の倍数）
    for (int j = 0; j < W; j += 16) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out + j));
        for (int t = 0; t < nm; t++) a = _mm256_sub_epi16(a, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(mr[t] + j)));
        for (int t = 0; t < np; t++) a = _mm256_add_epi16(a, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pr[t] + j)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + j), a);
    }
#else
    for (int j = 0; j < W; j++) {
        int32_t s = out[j];
        for (int t = 0; t < nm; t++) s -= mr[t][j];
        for (int t = 0; t < np; t++) s += pr[t][j];
        out[j] = (int16_t)s;  // 2 の補数の丸め（mod 2^16）
    }
#endif
}

// 視点 p の蓄積値を盤から作る（バイアス＋全部の行の和を int16 に丸める）
void full_one(const Network& net, const Board& b, int p, int16_t* out) {
    const View v = make_view(net, b, p);
    int idx[40];
    const int n = collect(v, b, idx);
    const int W = net.W;
    std::memcpy(out, net.ft_bias.data(), sizeof(int16_t) * W);
    for (int i = 0; i < n; i++) {
        const int16_t* row = &net.ft_weight[(size_t)idx[i] * W];
        for (int j = 0; j < W; j++) out[j] = (int16_t)(out[j] + row[j]);  // mod 2^16
    }
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

int16_t read_i16(const std::vector<uint8_t>& d, size_t at) {
    return (int16_t)(uint16_t)(d[at] | (d[at + 1] << 8));
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
    std::vector<uint8_t> d;
    if (!platform::read_whole_file(path, d)) {
        err = "ファイルを開けない: " + path;
        return false;
    }
    if (d.size() < 12) {
        err = "短すぎる";
        return false;
    }
    if (read_u32(d, 0) != kMagic) {
        err = "版が違う";
        return false;
    }
    const uint32_t file_hash = read_u32(d, 4);
    const size_t arch_len = read_u32(d, 8);
    if (arch_len > (1u << 20) || 12 + arch_len + 4 > d.size()) {
        err = "構造の文字列の長さが合わない";
        return false;
    }
    const size_t ft_at = 12 + arch_len;  // 特徴変換のハッシュ（構造の文字列は読み飛ばす）
    const uint32_t ft_file = read_u32(d, ft_at);

    // 1. 特徴と W：特徴変換のハッシュ（= 特徴のハッシュ ^ 2W）から決める
    int fi = -1, W = 0;
    for (int f = 0; f < 5 && fi < 0; f++) {
        for (int w = 32; w <= kMaxW; w += 32) {
            if ((kDefs[f].hash ^ (uint32_t)(2 * w)) == ft_file) { fi = f; W = w; break; }
        }
    }
    if (fi < 0) {
        err = "特徴変換のハッシュが合わない（特徴と幅が分からない）";
        return false;
    }
    const FeatureDef& f = kDefs[fi];
    const size_t net_at = ft_at + 4 + 2 * (size_t)W + 2 * (size_t)W * plane_of(f) * buckets_of(f);
    if (net_at + 4 > d.size()) {
        err = "ファイルが短すぎる";
        return false;
    }
    const uint32_t net_file = read_u32(d, net_at);

    // 2. 隠れ層の幅 L1・L2：ネットワークのハッシュ（と、ファイルの長さ・ファイル全体のハッシュ）で決める
    // ネットワークのハッシュは L1・L2 の組に対して衝突が多いので、ファイルの長さも合う組だけを数える
    int L1 = 0, L2 = 0, hits = 0;
    for (int a = 8; a <= kMaxL; a += 8) {
        for (int b = 8; b <= kMaxL; b += 8) {
            if (net_hash(W, a, b) != net_file) continue;
            if (file_size_of(arch_len, f, W, a, b) != d.size()) continue;
            hits++;
            L1 = a;
            L2 = b;
        }
    }
    if (hits == 0) {
        err = "ネットワークのハッシュが合わない（隠れ層の幅が分からない）";
        return false;
    }
    if (hits > 1) {
        err = "隠れ層の幅が1つに決まらない";
        return false;
    }
    if (file_hash != (ft_file ^ net_file)) {
        err = "ファイル全体のハッシュが合わない";
        return false;
    }

    // 3. 重みを読む（tests/nnue_nnbin.py の並び）
    net.feature = (Feature)fi;
    net.feature_name = f.name;
    net.W = W;
    net.L1 = L1;
    net.L2 = L2;
    net.feat_dim = plane_of(f) * buckets_of(f);
    net.in_pad = pad32(2 * W);
    net.l1_pad = pad32(L1);
    net.l2_pad = pad32(L2);
    net.network_name = std::string(f.lower) + "_" + std::to_string(W) + "x2-" + std::to_string(L1) + "-" + std::to_string(L2);

    size_t at = ft_at + 4;
    net.ft_bias.resize(W);
    for (int j = 0; j < W; j++, at += 2) net.ft_bias[j] = read_i16(d, at);
    net.ft_weight.resize((size_t)net.feat_dim * W);
    for (size_t i = 0; i < net.ft_weight.size(); i++, at += 2) net.ft_weight[i] = read_i16(d, at);
    at += 4;  // ネットワークのハッシュ
    net.b1.resize(L1);
    for (int j = 0; j < L1; j++, at += 4) net.b1[j] = (int32_t)read_u32(d, at);
    net.w1.resize((size_t)L1 * net.in_pad);
    for (size_t i = 0; i < net.w1.size(); i++, at++) net.w1[i] = (int8_t)d[at];
    net.b2.resize(L2);
    for (int j = 0; j < L2; j++, at += 4) net.b2[j] = (int32_t)read_u32(d, at);
    net.w2.resize((size_t)L2 * net.l1_pad);
    for (size_t i = 0; i < net.w2.size(); i++, at++) net.w2[i] = (int8_t)d[at];
    net.bo = (int32_t)read_u32(d, at);
    at += 4;
    net.wo.resize(net.l2_pad);
    for (int k = 0; k < net.l2_pad; k++, at++) net.wo[k] = (int8_t)d[at];
    return true;
}

void full_accumulator(const Network& net, const Board& board, int16_t* acc) {
    for (int p = 0; p < 2; p++) full_one(net, board, p, acc + (size_t)p * net.W);
}

void update_accumulator(const Network& net, const Board& after, const MoveInfo& mi,
                        const int16_t* prev, int16_t* next) {
    const int W = net.W;
    for (int p = 0; p < 2; p++) {
        int16_t* out = next + (size_t)p * W;
        // 自分の玉が動いた視点は、玉のマスと反転の向きが変わるので全計算
        if (mi.from >= 0 && (mi.piece & 15) == KING && mi.mover == p) {
            full_one(net, after, p, out);
            continue;
        }
        std::memcpy(out, prev + (size_t)p * W, sizeof(int16_t) * W);
        const View v = make_view(net, after, p);
        int minus[4], plus[4];
        int nm = 0, np = 0;
        if (mi.from >= 0) {
            // 移動元の駒（相手の玉なら HalfKA では差分で扱う。HalfKP では -1 で無し）と、取った駒
            minus[nm++] = board_index(v, mi.piece, halfkp_sq(mi.from));
            if (mi.captured) minus[nm++] = board_index(v, mi.captured, halfkp_sq(mi.to));
            plus[np++] = board_index(v, after.pieces[mi.to], halfkp_sq(mi.to));  // 移動先の駒（成った駒）
            // 取った駒は、取った側の持ち駒の、今の枚数の 1 枚目（枚数 − 1）になる。玉は持ち駒に入らない
            int t = mi.captured & 15;
            if (t > 8) t -= 8;  // 成駒は成る前の駒種
            if (mi.captured && t != KING) plus[np++] = hand_index(v, mi.mover, t, after.hand[mi.mover][t] - 1);
            // 玉を取られた局面（null の後に、相手の玉を取る手が生成されることがある）では、玉の数が減るので箱の駒が1つ増える
            if (mi.captured && t == KING && v.king) plus[np++] = v.base;
        }
        else {
            // 打った駒は、持ち駒の、打った後の枚数（打つ前の枚数の最後の1枚）を引く
            minus[nm++] = hand_index(v, mi.mover, mi.drop_type, after.hand[mi.mover][mi.drop_type]);
            plus[np++] = board_index(v, after.pieces[mi.to], halfkp_sq(mi.to));
        }
        // 無効な特徴（-1）を除く
        int m2 = 0, p2 = 0;
        for (int t = 0; t < nm; t++) if (minus[t] >= 0) minus[m2++] = minus[t];
        for (int t = 0; t < np; t++) if (plus[t] >= 0) plus[p2++] = plus[t];
        apply_rows(out, net, minus, m2, plus, p2);
    }
}

int32_t forward_raw(const Network& net, const int16_t* acc, int stm) {
    const int W = net.W;
    // 入力：手番側を先に、相手側を後に。clamp(0, 127) で uint8（2W 個。2W は 64 の倍数）
    alignas(32) uint8_t x[2 * kMaxW];
    const int16_t* own = acc + (size_t)stm * W;
    const int16_t* opp = acc + (size_t)(1 - stm) * W;
    for (int j = 0; j < W; j++) {
        x[j] = clamp127(own[j]);
        x[W + j] = clamp127(opp[j]);
    }
    // 隠れ層1：積和 ＋ bias を算術右シフト6 → clamp(0, 127)
    alignas(32) uint8_t h1[kMaxL];
    for (int j = 0; j < net.L1; j++) {
        const int32_t s = net.b1[j] + dot_u8_s8(x, &net.w1[(size_t)j * net.in_pad], net.in_pad);
        h1[j] = clamp127(s >> 6);
    }
    for (int j = net.L1; j < net.l1_pad; j++) h1[j] = 0;  // 第2層の入力の切り上げ分
    // 隠れ層2
    alignas(32) uint8_t h2[kMaxL];
    for (int j = 0; j < net.L2; j++) {
        const int32_t s = net.b2[j] + dot_u8_s8(h1, &net.w2[(size_t)j * net.l1_pad], net.l1_pad);
        h2[j] = clamp127(s >> 6);
    }
    for (int j = net.L2; j < net.l2_pad; j++) h2[j] = 0;  // 出力層の入力の切り上げ分
    return net.bo + dot_u8_s8(h2, net.wo.data(), net.l2_pad);
}

}  // namespace nnue
