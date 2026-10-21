// 重みファイルの読み込みと量子化、差分更新、推論（設計書 §F4・§E3・§E4）。
#include "network.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#if NUENAGON_SIMD_AVX2
#include <immintrin.h>
#endif

namespace raypair {
namespace {

constexpr size_t kHeaderSize = 180;
constexpr uint32_t kFormatVersion = 1;

// §F3.1 の組み合わせ（W・L1・L2 は版ごとに決まる）
struct NetworkId {
    const char* feature;
    const char* network;
    int dims, L1, L2, version;
};
constexpr NetworkId kIds[] = {
    {"RayPair_v0", "RayPair_v0Network", 15000, 16, 16, 0},
    {"RayPair_v1", "RayPair_v1Network", 115352, 32, 32, 1},
};

uint32_t read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t read_u64(const uint8_t* p) {
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

float read_f32(const uint8_t* p) {
    uint32_t u = read_u32(p);
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

// 固定長の文字列（NUL で終わる）を比べる。NUL が無ければ一致しない扱い
bool cstr_equals(const uint8_t* p, size_t len, const char* s) {
    const void* nul = std::memchr(p, 0, len);
    if (!nul) return false;
    return std::strcmp(reinterpret_cast<const char*>(p), s) == 0;
}

// CRC32（IEEE 802.3、zlib と同じ）
uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// 0 から遠い方への四捨五入（C++ の std::lround と同じ。参照実装と同じ式）
long long round_half_away(double x) {
    return (long long)std::copysign(std::floor(std::fabs(x) + 0.5), x);
}

// 本体の float32 の並び（§F4.2）から、量子化して重みに入れる。読む位置 pos は要素単位で進める
class BodyReader {
public:
    BodyReader(const uint8_t* body) : body_(body) {}
    float next() {
        float f = read_f32(body_ + pos_ * 4);
        pos_++;
        return f;
    }
private:
    const uint8_t* body_;
    size_t pos_ = 0;
};

}  // namespace

bool load_network(const std::string& path, Network& net, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "ファイルを開けない: " + path;
        return false;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    if (d.size() < kHeaderSize + 4) { err = "短すぎる"; return false; }
    if (std::memcmp(d.data(), "SHIKIBU\0", 8) != 0) { err = "マジックが違う"; return false; }
    if (read_u32(&d[8]) != kFormatVersion) { err = "形式バージョンが違う"; return false; }

    const NetworkId* id = nullptr;
    for (const NetworkId& c : kIds) {
        if (!cstr_equals(&d[12], 32, c.feature) || !cstr_equals(&d[44], 32, c.network)) continue;
        if ((int)read_u32(&d[76]) != c.dims || read_u32(&d[80]) != kW ||
            (int)read_u32(&d[84]) != c.L1 || (int)read_u32(&d[88]) != c.L2) continue;
        id = &c;
        break;
    }
    if (!id) { err = "特徴セット・ネットワーク・次元数・W・L1・L2 の組み合わせが合わない"; return false; }

    for (size_t i = 108; i < 172; i++) {
        if (d[i] != 0) { err = "予約領域が0でない"; return false; }
    }

    const int dims = id->dims, L1 = id->L1, L2 = id->L2;
    // 本体の要素数（§F4.2 の順）
    const uint64_t n_elems = (uint64_t)dims * kW + kW + (uint64_t)L1 * 2 * kW + L1 +
                             (uint64_t)L2 * L1 + L2 + (uint64_t)L2 + 1;
    const uint64_t body_len = read_u64(&d[172]);
    if (body_len != n_elems * 4) { err = "本体の長さが形と合わない"; return false; }
    if (d.size() != kHeaderSize + body_len + 4) { err = "ファイルの長さが合わない"; return false; }

    const uint8_t* body = &d[kHeaderSize];
    if (crc32(body, (size_t)body_len) != read_u32(&d[kHeaderSize + body_len])) {
        err = "CRC32 が合わない";
        return false;
    }

    // 量子化（§E4）。倍精度で掛けて、0 から遠い方へ丸める
    Network out;
    out.version = id->version;
    out.feature_set = id->feature;
    out.network_id = id->network;
    out.dims = dims;
    out.L1 = L1;
    out.L2 = L2;
    out.eval_scale = read_f32(&d[92]);

    BodyReader r(body);
    out.ft_weight.resize((size_t)dims * kW);
    for (auto& w : out.ft_weight) w = (int16_t)round_half_away((double)r.next() * 127.0);
    for (int j = 0; j < kW; j++) out.ft_bias[j] = (int16_t)round_half_away((double)r.next() * 127.0);

    auto clip64 = [](double w) {
        long long q = round_half_away(w * 64.0);
        return (int8_t)(q < -127 ? -127 : (q > 127 ? 127 : q));
    };
    out.l1_weight.resize((size_t)L1 * 2 * kW);
    for (auto& w : out.l1_weight) w = clip64((double)r.next());
    out.l1_bias.resize(L1);
    for (auto& b : out.l1_bias) b = (int32_t)round_half_away((double)r.next() * 127.0 * 64.0);
    out.l2_weight.resize((size_t)L2 * L1);
    for (auto& w : out.l2_weight) w = clip64((double)r.next());
    out.l2_bias.resize(L2);
    for (auto& b : out.l2_bias) b = (int32_t)round_half_away((double)r.next() * 127.0 * 64.0);
    out.out_weight.resize(L2);
    for (auto& w : out.out_weight) w = clip64((double)r.next());
    out.out_bias = (int32_t)round_half_away((double)r.next() * 127.0 * 64.0);

    net = std::move(out);
    return true;
}

void full_accumulator(const Network& net, const Board& board, Accumulator& acc) {
    std::vector<int> f;
    for (int p = 0; p < 2; p++) {
        compute_features(board, p, net.version, f);
        int32_t sum[kW];
        for (int j = 0; j < kW; j++) sum[j] = net.ft_bias[j];
        for (int i : f) {
            const int16_t* row = &net.ft_weight[(size_t)i * kW];
            for (int j = 0; j < kW; j++) sum[j] += row[j];
        }
        for (int j = 0; j < kW; j++) acc.v[p][j] = (int16_t)sum[j];
    }
}

namespace {

// 1視点の1手で変わる特徴の一覧（§E3.2）。引く特徴（minus）と足す特徴（plus）。固定の大きさで、確保はしない。
// 1手の上限：A1 ≤ 2、A2 ≤ 1、B1 は変わったマスを通る最大 8 本のラインに、それぞれ最大 8 組（指す前と後で各々）、
// B2 は変わったマスの最大 10 候補に、各 2 個（指す前と後で各々）。どちらも 160 で足りる。
constexpr int kRowCap = 160;
struct Rows {
    int minus[kRowCap];
    int plus[kRowCap];
    int nm = 0, np = 0;
};

// 指す前の駒番号。変わったマスは d の値、それ以外は今の盤（変わっていないので同じ）
int pre_pc_at(const ViewBoard& post, const ViewDelta& d, int sq) {
    for (int i = 0; i < d.n; i++) {
        if (d.sq[i] == sq) return d.pre_pc[i];
    }
    return post.pc[sq];
}

bool is_changed(const ViewDelta& d, int sq) {
    for (int i = 0; i < d.n; i++) {
        if (d.sq[i] == sq) return true;
    }
    return false;
}

// A1・A2：変わったマスの駒と、持ち駒の枚数の変化
void piece_and_hand_rows(const ViewBoard& post, const ViewDelta& d, Rows& r) {
    for (int i = 0; i < d.n; i++) {
        const int sq = d.sq[i];
        const int pre = d.pre_pc[i], now = post.pc[sq];
        if (pre == now) continue;
        if (pre >= 0) r.minus[r.nm++] = piece_feature(pre, sq);
        if (now >= 0) r.plus[r.np++] = piece_feature(now, sq);
    }
    for (int side = 0; side < 2; side++) {
        for (int h = 0; h < 7; h++) {
            const int a = d.pre_hand[side][h], b = post.hand[side][h];
            for (int k = a; k < b; k++) r.plus[r.np++] = hand_feature(side, h, k);
            for (int k = b; k < a; k++) r.minus[r.nm++] = hand_feature(side, h, k);
        }
    }
}

// B1：変わったマスを通るライン1本の組。組は、間に駒が無く隣り合う2つの駒（§F2.1）。
// 指す前と指した後で、変わったマスを含む区間（両端の駒を含む）の組だけが変わる。
// それ以外の区間の組は、両端と間の駒が変わらないので、指す前と後で同じ。
// 1回の走査で、指す前（minus）と指した後（plus）の2つの列を追う。
void b1_line_rows(const ViewBoard& post, const ViewDelta& d, int version, int line, Rows& r) {
    const int* sq = kLineTable.sq[line];
    const int n = kLineTable.n[line];
    const int axis = kLineTable.axis[line];
    int pre_prev = -1, pre_prev_pc = -1;    // 指す前：直前の駒のマスと駒番号
    int post_prev = -1, post_prev_pc = -1;  // 指した後
    bool pre_changed = false, post_changed = false;  // 直前の駒（その駒を含む）から今のマスまでに変わったマスがあるか
    for (int i = 0; i < n; i++) {
        const int s = sq[i];
        const bool ch = is_changed(d, s);
        pre_changed = pre_changed || ch;
        post_changed = post_changed || ch;
        const int pre_pc = pre_pc_at(post, d, s), post_pc = post.pc[s];
        if (pre_pc >= 0) {
            if (pre_prev >= 0 && pre_changed) r.minus[r.nm++] = b1_feature(version, axis, pre_prev, pre_prev_pc, s, pre_pc);
            pre_prev = s;
            pre_prev_pc = pre_pc;
            pre_changed = ch;
        }
        if (post_pc >= 0) {
            if (post_prev >= 0 && post_changed) r.plus[r.np++] = b1_feature(version, axis, post_prev, post_prev_pc, s, post_pc);
            post_prev = s;
            post_prev_pc = post_pc;
            post_changed = ch;
        }
    }
}

// B2：桂1枚の利き（指す前なら before = true）。append_knight_features と同じ規則
void knight_rows(int version, const ViewBoard& post, const ViewDelta& d, int sq, bool before, Rows& r) {
    const int pc = before ? pre_pc_at(post, d, sq) : post.pc[sq];
    if (pc != 2 && pc != 16) return;  // 成っていない桂。2 は視点側、16 は相手側
    const int s = (pc == 2) ? 0 : 1;
    const int y = sq / 9, x = sq % 9;
    const int ty = (s == 0) ? y - 2 : y + 2;
    if (ty < 0 || ty > 8) return;
    for (int lr = 0; lr < 2; lr++) {
        const int tx = (lr == 0) ? x - 1 : x + 1;
        if (tx < 0 || tx > 8) continue;
        const int target = before ? pre_pc_at(post, d, ty * 9 + tx) : post.pc[ty * 9 + tx];
        if (target < 0) continue;
        const int f = knight_feature(version, s, target, lr);
        if (before) r.minus[r.nm++] = f;
        else r.plus[r.np++] = f;
    }
}

// B2：利きが変わり得る桂は、変わったマス自身と、変わったマスに飛べる（桂の跳び方の位置の）マス。
// それ以外の桂は、自分のマスも跳び先も変わらないので、指す前と後で同じ。
void knight_delta_rows(int version, const ViewBoard& post, const ViewDelta& d, Rows& r) {
    int cand[10];
    int nc = 0;
    auto add = [&](int s) {
        for (int i = 0; i < nc; i++) {
            if (cand[i] == s) return;
        }
        cand[nc++] = s;
    };
    for (int i = 0; i < d.n; i++) {
        const int c = d.sq[i];
        add(c);
        const int y = c / 9, x = c % 9;
        for (int dy = -2; dy <= 2; dy += 4) {
            for (int dx = -1; dx <= 1; dx += 2) {
                const int ny = y + dy, nx = x + dx;
                if (ny >= 0 && ny <= 8 && nx >= 0 && nx <= 8) add(ny * 9 + nx);
            }
        }
    }
    for (int i = 0; i < nc; i++) {
        knight_rows(version, post, d, cand[i], true, r);
        knight_rows(version, post, d, cand[i], false, r);
    }
}

// 蓄積値 v（W 個）に、minus の行を引き、plus の行を足す。int16 の足し引きは mod 2^16 なので、順序によらず同じ結果
void apply_rows(int16_t* v, const int16_t* ft, const Rows& r) {
#if NUENAGON_SIMD_AVX2
    // 蓄積値の 128 要素を ymm レジスタ 8 本に載せたまま、全部の行を足し引きして、最後に1回だけ書き戻す
    __m256i a[8];
    for (int k = 0; k < 8; k++) a[k] = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + 16 * k));
    for (int t = 0; t < r.nm; t++) {
        const __m256i* row = reinterpret_cast<const __m256i*>(ft + (size_t)r.minus[t] * kW);
        for (int k = 0; k < 8; k++) a[k] = _mm256_sub_epi16(a[k], _mm256_loadu_si256(row + k));
    }
    for (int t = 0; t < r.np; t++) {
        const __m256i* row = reinterpret_cast<const __m256i*>(ft + (size_t)r.plus[t] * kW);
        for (int k = 0; k < 8; k++) a[k] = _mm256_add_epi16(a[k], _mm256_loadu_si256(row + k));
    }
    for (int k = 0; k < 8; k++) _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + 16 * k), a[k]);
#else
    for (int t = 0; t < r.nm; t++) {
        const int16_t* row = ft + (size_t)r.minus[t] * kW;
        for (int j = 0; j < kW; j++) v[j] = (int16_t)(v[j] - row[j]);
    }
    for (int t = 0; t < r.np; t++) {
        const int16_t* row = ft + (size_t)r.plus[t] * kW;
        for (int j = 0; j < kW; j++) v[j] = (int16_t)(v[j] + row[j]);
    }
#endif
}

// x（uint8）と w（int8）の内積。n は 32 の倍数。積の和は int32 で足す（両版で同じ値）
int32_t dot_u8_s8(const uint8_t* x, const int8_t* w, int n) {
#if NUENAGON_SIMD_AVX2
    // maddubs の 2 つの積の和は |和| ≤ 2 × 127 × 127 = 32,258 で、int16 に収まる（飽和しない）
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

}  // namespace

void update_accumulator(const Network& net, int p, const ViewBoard& post, const ViewDelta& d, Accumulator& acc) {
    Rows r;
    piece_and_hand_rows(post, d, r);

    // B1：変わったマスを通るラインは、高々 8 本（マス2つ × 4 軸）。同じラインは1回だけ
    int lines[8];
    int nl = 0;
    for (int i = 0; i < d.n; i++) {
        for (int k = 0; k < 4; k++) {
            const int line = kLinesOf.line[d.sq[i]][k];
            bool seen = false;
            for (int j = 0; j < nl; j++) seen = seen || (lines[j] == line);
            if (!seen) lines[nl++] = line;
        }
    }
    for (int j = 0; j < nl; j++) b1_line_rows(post, d, net.version, lines[j], r);

    knight_delta_rows(net.version, post, d, r);

    apply_rows(acc.v[p], net.ft_weight.data(), r);
}

int32_t forward_raw(const Network& net, const Accumulator& acc, int stm) {
    // 手番側を先に連結し、clamp(0, 127) で uint8 にする
    alignas(32) uint8_t x[2 * kW];
    for (int j = 0; j < kW; j++) {
        int a = acc.v[stm][j], b = acc.v[1 - stm][j];
        x[j] = (uint8_t)(a < 0 ? 0 : (a > 127 ? 127 : a));
        x[kW + j] = (uint8_t)(b < 0 ? 0 : (b > 127 ? 127 : b));
    }

    // 隠れ層1：int32 の積和 ＋ bias を算術右シフト6 → clamp(0, 127)
    const int L1 = net.L1, L2 = net.L2;
    int32_t h1[32], h2[32];
    for (int j = 0; j < L1; j++) {
        int32_t s = net.l1_bias[j] + dot_u8_s8(x, &net.l1_weight[(size_t)j * 2 * kW], 2 * kW);
        s >>= 6;
        h1[j] = s < 0 ? 0 : (s > 127 ? 127 : s);
    }
    for (int j = 0; j < L2; j++) {
        int32_t s = net.l2_bias[j];
        const int8_t* row = &net.l2_weight[(size_t)j * L1];
        for (int k = 0; k < L1; k++) s += (int32_t)row[k] * h1[k];
        s >>= 6;
        h2[j] = s < 0 ? 0 : (s > 127 ? 127 : s);
    }

    int32_t raw = net.out_bias;
    for (int k = 0; k < L2; k++) raw += (int32_t)net.out_weight[k] * h2[k];
    return raw;
}

long long cp_of(int32_t raw, double eval_scale) {
    return round_half_away((double)raw * eval_scale / 8128.0);
}

}  // namespace raypair
