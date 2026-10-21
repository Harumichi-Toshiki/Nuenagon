// 重みファイルの読み込みと量子化、差分更新、推論（設計書 §F4・§E3・§E4）。
#include "network.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

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

void update_accumulator(const Network& net, int p, const ViewBoard& pre, const ViewBoard& post, Accumulator& acc) {
    // 駒が変わったマス（移動元・移動先・取られた駒のマス）。引く特徴と足す特徴を集める
    std::vector<int> minus, plus;
    bool line_hit[kLines] = {};
    for (int sq = 0; sq < 81; sq++) {
        if (pre.pc[sq] == post.pc[sq]) continue;
        if (pre.pc[sq] >= 0) minus.push_back(piece_feature(pre.pc[sq], sq));    // A1
        if (post.pc[sq] >= 0) plus.push_back(piece_feature(post.pc[sq], sq));
        for (int k = 0; k < 4; k++) line_hit[kLinesOf.line[sq][k]] = true;  // B1 の対象（重複は1回）
    }
    // A2：持ち駒の枚数が a → b のとき、b > a なら k = a..b−1 を足し、b < a なら k = b..a−1 を引く
    for (int side = 0; side < 2; side++) {
        for (int h = 0; h < 7; h++) {
            int a = pre.hand[side][h], b = post.hand[side][h];
            for (int k = a; k < b; k++) plus.push_back(hand_feature(side, h, k));
            for (int k = b; k < a; k++) minus.push_back(hand_feature(side, h, k));
        }
    }
    // B1：対象のラインは、指す前の盤面の特徴を全部引き、指した後の盤面の特徴を全部足す
    for (int line = 0; line < kLines; line++) {
        if (!line_hit[line]) continue;
        append_line_b1(pre, net.version, line, minus);
        append_line_b1(post, net.version, line, plus);
    }
    // B2：桂の利きは、指す前の全部を引き、指した後の全部を足す
    append_knight_features(pre, net.version, minus);
    append_knight_features(post, net.version, plus);

    for (int i : minus) {
        const int16_t* row = &net.ft_weight[(size_t)i * kW];
        for (int j = 0; j < kW; j++) acc.v[p][j] = (int16_t)(acc.v[p][j] - row[j]);
    }
    for (int i : plus) {
        const int16_t* row = &net.ft_weight[(size_t)i * kW];
        for (int j = 0; j < kW; j++) acc.v[p][j] = (int16_t)(acc.v[p][j] + row[j]);
    }
}

int32_t forward_raw(const Network& net, const Accumulator& acc, int stm) {
    // 手番側を先に連結し、clamp(0, 127) で uint8 にする
    uint8_t x[2 * kW];
    for (int j = 0; j < kW; j++) {
        int a = acc.v[stm][j], b = acc.v[1 - stm][j];
        x[j] = (uint8_t)(a < 0 ? 0 : (a > 127 ? 127 : a));
        x[kW + j] = (uint8_t)(b < 0 ? 0 : (b > 127 ? 127 : b));
    }

    // 隠れ層1：int32 の積和 ＋ bias を算術右シフト6 → clamp(0, 127)
    const int L1 = net.L1, L2 = net.L2;
    int32_t h1[32], h2[32];
    for (int j = 0; j < L1; j++) {
        int32_t s = net.l1_bias[j];
        const int8_t* row = &net.l1_weight[(size_t)j * 2 * kW];
        for (int k = 0; k < 2 * kW; k++) s += (int32_t)row[k] * x[k];
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
