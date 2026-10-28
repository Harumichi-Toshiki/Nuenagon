// αβ探索の実装（設計書 §E5、§E11）。反復深化、aspiration window、PVS、置換表、null move、LMR、王手延長、
// 静止探索、指し手の並べ替え（SEE・履歴・連続履歴・カウンター手）、枝刈り（RFP・LMP・SEE・razoring）、
// 1手詰め、千日手・最大手数・詰み、時間管理、Lazy SMP。
#include "search/search.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

#include "eval/raypair/raypair.h"
#include "mate/dfpn.h"
#include "platform/cpu_affinity.h"

namespace search {

// ---------- 小さな道具 ----------

namespace {

using Clock = std::chrono::steady_clock;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

constexpr int kCheckInterval = 1024;     // 時間・ノード数・検査は、この間隔のノードごとに行う
constexpr int kHistSize = 89;            // 履歴の添字：盤上の手は from（0..80）、駒打ちは 81 + 駒種
constexpr int kKeys = 16 * 81;           // 手の鍵の数（駒種（成り込み）×到達マス。連続履歴と、カウンター手で使う）
constexpr int kHistLimit = 16384;        // 履歴の値の上限（減衰つきの更新。§E11）
constexpr int kMaxQuiets = 64;           // 減点（履歴の malus）に使う、試した静かな手の数
constexpr long long kMateProbeNodes = 300000;  // 根の詰み探索に使うノード数の上限
constexpr int kMateProbeTableSize = 1 << 16;   // 根の詰み探索の表の大きさ（大きい表は確保だけで数十ms かかる）
constexpr long long kMateProbeMsNoLimit = 200; // 締め切りの無い探索での詰み探索の時間（ms）
constexpr int kQLimit = 16;              // 静止探索の手数の上限（駒を取る手だけ）
constexpr int kQCheckLimit = 4;          // 静止探索で王手を受けている局面の手数の上限
constexpr int kNoEval = -kInf;           // 静的評価が無い（王手を受けている節点）

// 駒の価値（並べ替え・SEE 用。駒種 0..15。成駒は元の駒＋少し）
constexpr int kPieceValue[16] = {0, 100, 300, 350, 450, 500, 800, 900, 10000, 420, 420, 420, 420, 0, 1000, 1100};

bool same_move(const Move& a, const Move& b) {
    return a.from == b.from && a.to == b.to && a.type == b.type && a.is_promote == b.is_promote && a.is_drop == b.is_drop;
}

bool is_capture(const Board& b, const Move& m) { return !m.is_drop && b.pieces[m.to] != 0; }

int hist_index(const Move& m) { return m.is_drop ? 81 + m.type : m.from; }

// 手の鍵：駒種（成りを含む）×到達マス。連続履歴の添字になる
int piece_key(const Move& m) { return (m.is_promote ? m.type + 8 : m.type) * 81 + m.to; }

// 減衰つきの履歴の更新：値は ±kHistLimit に収まり、同じ手が続くと飽和する
template <class T>
void gravity(T& h, int bonus) {
    const int v = h;
    h = (T)std::clamp(v + bonus - v * std::abs(bonus) / kHistLimit, -kHistLimit, kHistLimit);
}

// LMR の削減量の表（§E11）：log(深さ) × log(手の順番)。0 は削減なし
int lmr_table(int depth, int move_no) {
    static const auto table = [] {
        std::array<std::array<int, 64>, 64> t{};
        for (int d = 1; d < 64; d++) {
            for (int m = 1; m < 64; m++) t[d][m] = (int)(0.75 + std::log((double)d) * std::log((double)m) / 2.25);
        }
        return t;
    }();
    return table[std::min(depth, 63)][std::min(move_no, 63)];
}

// 王の周り（chebyshev の距離 2 以内）か、同じ筋・段・斜めにあるマスか（王手になりうる手の目安）
bool near_line(int sq, int kx, int ky) {
    const int dx = std::abs(sq % 9 - kx), dy = std::abs(sq / 9 - ky);
    return dx == 0 || dy == 0 || dx == dy || std::max(dx, dy) <= 2;
}

// 千分率の置換表の値（キー用）を、手の 21bit に詰める
uint64_t pack_move(const Move& m) {
    uint64_t v = 1;  // 有効フラグ
    v |= (uint64_t)(m.is_drop ? 0 : (m.from & 127)) << 1;
    v |= (uint64_t)(m.to & 127) << 8;
    v |= (uint64_t)(m.type & 15) << 15;
    v |= (uint64_t)(m.is_promote ? 1 : 0) << 19;
    v |= (uint64_t)(m.is_drop ? 1 : 0) << 20;
    return v;
}

Move unpack_move(uint64_t v) {
    Move m{};
    m.from = (int)((v >> 1) & 127);
    m.to = (int)((v >> 8) & 127);
    m.type = (int)((v >> 15) & 15);
    m.is_promote = ((v >> 19) & 1) != 0;
    m.is_drop = ((v >> 20) & 1) != 0;
    if (m.is_drop) m.from = -1;
    return m;
}

// 詰みの値の換算：置換表には「その局面から」の詰みで保存し、取り出すときに根からの手数に直す（§E5.3）
int to_tt(int score, int ply) {
    if (score >= kMateBound) return score + ply;
    if (score <= -kMateBound) return score - ply;
    return score;
}
int from_tt(int score, int ply) {
    if (score >= kMateBound) return score - ply;
    if (score <= -kMateBound) return score + ply;
    return score;
}

// USI の score 表記：詰みは手数（ply。勝ちは正の奇数、詰まされるのは負の偶数。N4.1）、それ以外は cp
std::string score_text(int s) {
    if (s >= kMateBound) return "mate " + std::to_string(kMate - s);
    if (s <= -kMateBound) return "mate -" + std::to_string(kMate + s);
    return "cp " + std::to_string(s);
}

}  // namespace

// ---------- 置換表 ----------

void TranspositionTable::resize(int mb) {
    size_t bytes = (size_t)std::max(1, mb) * 1024 * 1024;
    size_ = std::max<size_t>(1024, bytes / sizeof(Slot));
    slots_.reset(new Slot[size_]);
    gen_ = 0;
}

void TranspositionTable::clear() {
    for (size_t i = 0; i < size_; i++) {
        slots_[i].key_xor.store(0, std::memory_order_relaxed);
        slots_[i].data.store(0, std::memory_order_relaxed);
    }
    gen_ = 0;
}

bool TranspositionTable::probe(uint64_t key, TTEntry& e) const {
    e = TTEntry{};
    if (!size_) return false;
    const Slot& s = slots_[key % size_];
    const uint64_t d = s.data.load(std::memory_order_relaxed);
    const uint64_t k = s.key_xor.load(std::memory_order_relaxed);
    if ((k ^ d) != key || d == 0) return false;
    e.found = true;
    const uint64_t mv = d & ((1ull << 21) - 1);
    e.has_move = mv & 1;
    e.move = unpack_move(mv);
    e.score = (int)((d >> 21) & 0xffff) - 32768;
    e.depth = (int)((d >> 37) & 0xff);
    e.bound = (int)((d >> 45) & 3);
    return true;
}

void TranspositionTable::store(uint64_t key, int depth, int score, int bound, const Move& m, bool has_move) {
    if (!size_) return;
    Slot& s = slots_[key % size_];
    const uint64_t old_d = s.data.load(std::memory_order_relaxed);
    const uint64_t old_k = s.key_xor.load(std::memory_order_relaxed);
    const bool old_valid = old_d != 0;
    const int old_depth = (int)((old_d >> 37) & 0xff);
    const int old_gen = (int)((old_d >> 47) & 0xff);
    const bool same = old_valid && (old_k ^ old_d) == key;
    // 同じ局面、古い世代、または深さが同じか深いときに置き換える
    if (old_valid && !same && old_gen == gen_ && depth < old_depth) return;

    // 同じ局面で手が無い保存のときは、前の手を残す
    uint64_t mv = (has_move ? pack_move(m) : 0);
    if (!has_move && same) mv = old_d & ((1ull << 21) - 1);
    const uint64_t d = mv
                     | ((uint64_t)(score + 32768) & 0xffff) << 21
                     | ((uint64_t)(depth & 0xff)) << 37
                     | ((uint64_t)(bound & 3)) << 45
                     | ((uint64_t)gen_) << 47;
    s.data.store(d, std::memory_order_relaxed);
    s.key_xor.store(key ^ d, std::memory_order_relaxed);
}

int TranspositionTable::hashfull() const {
    const size_t n = std::min<size_t>(size_, 1000);
    if (!n) return 0;
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        const uint64_t d = slots_[i].data.load(std::memory_order_relaxed);
        if (d != 0 && (int)((d >> 47) & 0xff) == gen_) used++;
    }
    return (int)(used * 1000 / n);
}

// ---------- ヒントの表（§E5.5） ----------

void HintTable::set(uint64_t key, std::vector<HintEntry> entries) {
    if (!table_.count(key)) {
        order_.push_back(key);
        while (order_.size() > kMaxEntries) {
            table_.erase(order_.front());
            order_.pop_front();
        }
    }
    table_[key] = std::move(entries);
}

const std::vector<HintEntry>* HintTable::find(uint64_t key) const {
    auto it = table_.find(key);
    return it == table_.end() ? nullptr : &it->second;
}

// ---------- スレッドプール（§E5.2） ----------

ThreadPool::ThreadPool(int n, std::vector<int> cpus) : cpus_(std::move(cpus)) {
    for (int i = 0; i < n; i++) workers_.emplace_back([this, i] { loop(i); });
    // 固定が済むまで待つ（固定に失敗したワーカーの数を、作った直後に分かるようにする）
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this, n] { return started_ == n; });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::loop(int index) {
    if (!cpus_.empty()) {
        std::string err;
        if (!cpu::pin_current_thread(cpus_[(index + 1) % cpus_.size()], err)) pin_failures_++;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        started_++;
    }
    cv_.notify_all();
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return quit_ || !tasks_.empty(); });
            if (tasks_.empty()) return;  // quit_ のとき、残りの作業は捨てる
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

// ---------- 探索（1スレッドぶん） ----------

namespace {

class Worker {
public:
    Worker(Job::Shared& sh, const Game& root, int id)
        : sh_(sh), game_(root), id_(id), buf_(new MoveBuf[kMaxPly + 1]), history_(2 * kHistSize * 81, 0),
          cont_(kKeys * kKeys, 0), counter_(kKeys, Move{-1, -1, 0, false, false}) {
        std::memset(killers_, 0, sizeof killers_);
        std::memset(pv_len_, 0, sizeof pv_len_);
        std::fill(cur_key_, cur_key_ + kMaxPly + 2, -1);
        std::fill(eval_stack_, eval_stack_ + kMaxPly + 2, kNoEval);
        pv_.assign(kMaxPly * kMaxPly, Move{});
    }

    // 反復深化（§E5.1）。rm は根の手（並べ替えられる）。報告する深さごとに on_depth を呼ぶ
    template <class F>
    void iterate(std::vector<RootMove>& rm, int max_depth, const F* on_depth);

public:
    int negamax(int depth, int alpha, int beta, int ply, bool allow_null);
    int qsearch(int alpha, int beta, int ply, int qd);
    int search_root(int depth, int alpha, int beta, int pv_idx, std::vector<RootMove>& rm);

    bool aborted();                 // 止める条件（停止・時間・ノード数・差分更新の検査）を見る
    long long total_nodes() const { return sh_.nodes.load(std::memory_order_relaxed) + pending_; }
    int evaluate() const;
    int draw_score() const { return sh_.opt.draw_value; }
    bool has_non_pawn_material() const;
    int see(const Move& m) const;                 // 静的交換評価（SEE）。手を指したときの駒の損得（内部の単位）
    int quiet_score(const Move& m, int ply) const;  // 静かな手の並べ替えの点数（履歴＋連続履歴）
    int order_score(const Move& m, const Move& tt_move, bool has_tt, int ply, const std::vector<HintEntry>* hint) const;
    void record_quiet_cutoff(const Move& m, int depth, int ply, const Move* quiets, int nq);
    void update_quiet(const Move& m, int bonus, int ply);
    bool find_mate1(const Move* moves, int n, Move& out) const;  // 1手で詰む手（§E11。自玉の安全と、詰みを確かめる）
    bool has_reply() const;                       // 今の局面（手番側）に合法手があるか（打ち歩詰めの判定）
    bool quiet_drop_far(const Move& m) const;     // 打つ手が、両方の玉から遠く、どの駒にも利かないか（目安）
    bool king_boxed() const;                      // 相手の玉の逃げ道が1つ以下か（詰みが近い節点の目安）
    Move pv_at(int ply, int i) const { return pv_[ply * kMaxPly + i]; }
    void set_pv(int ply, const Move& m);

    Job::Shared& sh_;
    Game game_;
    int id_;
    long long pending_ = 0;          // まだ共有カウンタに足していないノード数
    int seldepth_ = 0;
    // 指し手の一覧と、並べ替えの点数の作業領域（ノードの深さごと。確保は作るときだけ）
    struct MoveBuf {
        Move moves[MAX_LEGAL_MOVES];
        int score[MAX_LEGAL_MOVES];
    };
    std::unique_ptr<MoveBuf[]> buf_;
    std::vector<int> history_;       // 履歴 [手番][履歴の添字][到達マス]（減衰つき）
    std::vector<int16_t> cont_;      // 連続履歴 [直前の手の鍵][この手の鍵]（減衰つき。§E11）
    std::vector<Move> counter_;      // カウンター手 [直前の手の鍵]（`from` が -1 なら無し）
    Move killers_[kMaxPly][2];
    int cur_key_[kMaxPly + 2];       // ply に至った手の鍵（null は -1）。連続履歴に使う
    int eval_stack_[kMaxPly + 2];    // ply の静的評価（王手を受けているときは kNoEval）。改善の判定に使う
    std::vector<Move> pv_;           // 三角の読み筋 [ply][i]
    int pv_len_[kMaxPly];
};

bool Worker::aborted() {
    if (sh_.stop.load(std::memory_order_relaxed)) return true;
    if (++pending_ < kCheckInterval) return false;
    sh_.nodes.fetch_add(pending_, std::memory_order_relaxed);
    pending_ = 0;
    if (sh_.node_limit > 0 && sh_.nodes.load(std::memory_order_relaxed) >= sh_.node_limit) {
        sh_.stop = true;
        return true;
    }
    const long long dl = sh_.deadline_ms.load(std::memory_order_relaxed);
    if (dl > 0 && now_ms() >= dl) {
        sh_.stop = true;
        return true;
    }
    // §E3.4：差分更新の蓄積値を全計算と比べる
    if (sh_.opt.debug_check && (sh_.ev.net || sh_.ev.hk) && !game_.accumulator_matches_full()) {
        if (sh_.sink) {
            sh_.sink("info string accumulator mismatch " + game_.board.to_sfen_key() + " " + game_.last_move_usi());
        }
        sh_.stop = true;
        return true;
    }
    return false;
}

int Worker::evaluate() const {
    const int32_t raw = game_.raw_eval();
    // HalfKP は raw を FV_SCALE で割って 0 の方へ切り捨てる（§E10）。RayPair は eval_scale で換算する
    long long cp = sh_.ev.hk ? (long long)(raw / sh_.ev.fv_scale) : raypair::cp_of(raw, sh_.ev.scale);
    cp = std::max<long long>(-kMateCp, std::min<long long>(kMateCp, cp));
    return (int)cp;
}

bool Worker::has_non_pawn_material() const {
    const int turn = game_.board.turn;
    for (int sq = 0; sq < 81; sq++) {
        const int v = game_.board.pieces[sq];
        if (!v) continue;
        const int color = (v > 16) ? WHITE : BLACK;
        if (color != turn) continue;
        const int t = v & 15;
        if (t != 1 && t != 8) return true;  // 歩と玉以外
    }
    for (int t = 2; t <= 7; t++) {
        if (game_.board.hand[turn][t] > 0) return true;
    }
    return false;
}

// 静的交換評価（SEE、§E11）。手を指した側から見た、その取り合いの損得。取る駒が無ければ 0 から始まる。
// 最も安い駒から順に取り合い、途中で止めてもよい側が損をしない値を返す（swap アルゴリズム）。
int Worker::see(const Move& m) const {
    const Board& b = game_.board;
    const int to = m.to;
    int gain[40];
    int d = 0;
    gain[0] = is_capture(b, m) ? kPieceValue[b.pieces[to] & 15] : 0;
    int on_to = kPieceValue[m.is_promote ? m.type + 8 : m.type];  // 今 to にある駒（次に取られる駒）の価値
    Position p = b.bit_pos;
    p.do_move(m);
    int side = 1 - b.turn;
    for (;;) {
        const Bitboard att = p.get_attackers_to(to, side);
        if (!att.is_not_empty()) break;
        // 相手側で最も安い駒から取る
        int best_t = 0, best_v = 1 << 30, from = -1;
        for (int t = 1; t <= 15; t++) {
            if (kPieceValue[t] == 0 || kPieceValue[t] >= best_v) continue;
            Bitboard bb = att & p.type_bb[t];
            if (!bb.is_not_empty()) continue;
            best_t = t;
            best_v = kPieceValue[t];
            from = bb.pop_lsb();
        }
        if (from < 0) break;
        d++;
        gain[d] = on_to - gain[d - 1];
        on_to = best_v;
        p.do_move(Move{from, to, best_t, false, false});
        side = 1 - side;
    }
    // 取り返しの連鎖を、それぞれの側が損をしない方向に畳む
    for (int k = d; k > 0; k--) gain[k - 1] = -std::max(-gain[k - 1], gain[k]);
    return gain[0];
}

int Worker::quiet_score(const Move& m, int ply) const {
    const Board& b = game_.board;
    int s = history_[(b.turn * kHistSize + hist_index(m)) * 81 + m.to];
    if (sh_.opt.search_cont_hist) {
        const int k = piece_key(m);
        for (int p = ply; p >= ply - 1 && p >= 0; p--) {
            if (cur_key_[p] >= 0) s += cont_[cur_key_[p] * kKeys + k];
        }
    }
    return s;
}

// 並べ替えの点数：置換表の手 > 得な駒取り（SEE ≥ 0）> 成り > ヒント > キラー・カウンター手 > 静かな手（履歴）> 損な駒取り
int Worker::order_score(const Move& m, const Move& tt_move, bool has_tt, int ply, const std::vector<HintEntry>* hint) const {
    if (has_tt && same_move(m, tt_move)) return 1 << 30;
    const Board& b = game_.board;
    if (is_capture(b, m)) {
        const int victim = kPieceValue[b.pieces[m.to] & 15];
        const int attacker = kPieceValue[b.pieces[m.from] & 15];
        if (!sh_.opt.search_order_see) return (1 << 28) + victim * 8 - attacker / 8;
        if (see(m) >= 0) return (1 << 28) + victim * 8 - attacker / 8;
        return -(1 << 29) + victim * 8 - attacker / 8;
    }
    if (m.is_promote) return (1 << 27) + 1;
    if (hint) {
        for (const HintEntry& h : *hint) {
            if (same_move(h.move, m)) return (1 << 26) + (int)(h.prob * 100000);
        }
    }
    if (same_move(killers_[ply][0], m) || same_move(killers_[ply][1], m)) return 1 << 25;
    if (sh_.opt.search_counter_move && cur_key_[ply] >= 0 && same_move(counter_[cur_key_[ply]], m)) return 1 << 25;
    return quiet_score(m, ply);
}

void Worker::update_quiet(const Move& m, int bonus, int ply) {
    gravity(history_[(game_.board.turn * kHistSize + hist_index(m)) * 81 + m.to], bonus);
    if (!sh_.opt.search_cont_hist) return;
    const int k = piece_key(m);
    for (int p = ply; p >= ply - 1 && p >= 0; p--) {
        if (cur_key_[p] >= 0) gravity(cont_[cur_key_[p] * kKeys + k], bonus);
    }
}

// 静かな手で β カットが起きたとき：キラー・カウンター手を覚え、この手を強め、先に試した静かな手を弱める（§E11）
void Worker::record_quiet_cutoff(const Move& m, int depth, int ply, const Move* quiets, int nq) {
    if (!same_move(killers_[ply][0], m)) {
        killers_[ply][1] = killers_[ply][0];
        killers_[ply][0] = m;
    }
    if (sh_.opt.search_counter_move && cur_key_[ply] >= 0) counter_[cur_key_[ply]] = m;
    if (!sh_.opt.search_history_gravity) {  // N5 の履歴（加算のみ。試した手の減点はしない）
        int& h = history_[(game_.board.turn * kHistSize + hist_index(m)) * 81 + m.to];
        h = std::min(h + depth * depth, 1 << 20);
        return;
    }
    const int bonus = std::min(depth * depth, 1600);
    update_quiet(m, bonus, ply);
    for (int i = 0; i < nq; i++) update_quiet(quiets[i], -bonus, ply);
}

void Worker::set_pv(int ply, const Move& m) {
    pv_[ply * kMaxPly] = m;
    const int child = pv_len_[ply + 1];
    for (int i = 0; i < child; i++) pv_[ply * kMaxPly + 1 + i] = pv_[(ply + 1) * kMaxPly + i];
    pv_len_[ply] = 1 + child;
}

bool Worker::has_reply() const {
    Move tmp[MAX_LEGAL_MOVES];
    return game_.gen_legal(tmp) != tmp;
}

bool Worker::quiet_drop_far(const Move& m) const {
    const Board& b = game_.board;
    const int us = b.turn, them = 1 - us;
    const int tx = m.to % 9, ty = m.to / 9;
    for (int c : {us, them}) {
        const int k = b.king_sq[c];
        if (k < 0 || std::max(std::abs(k % 9 - tx), std::abs(k / 9 - ty)) < 3) return false;
    }
    for (int sq = 0; sq < 81; sq++) {
        const int v = b.pieces[sq];
        if (!v || ((v > 16) ? WHITE : BLACK) != them) continue;
        // 同じ筋・段・斜め、または近くに相手の駒があれば、利くかもしれないとみなす
        if (near_line(sq, tx, ty)) return false;
    }
    return true;
}

// 相手の玉の逃げ道（隣で、相手の駒がなく、こちらの利きも無いマス）が1つ以下か。
// 玉が囲まれている節点では、静的評価が詰みを隠すので、RFP は使わない（SearchRFPGuard）。
bool Worker::king_boxed() const {
    const Board& b = game_.board;
    const int us = b.turn, them = 1 - us;
    const int k = b.king_sq[them];
    if (k < 0) return false;
    int flights = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const int x = k % 9 + dx, y = k / 9 + dy;
            if ((!dx && !dy) || x < 0 || x > 8 || y < 0 || y > 8) continue;
            const int sq = y * 9 + x;
            if (b.pieces[sq] && ((b.pieces[sq] > 16) ? WHITE : BLACK) == them) continue;
            if (!b.is_attacked(sq, us) && ++flights > 1) return false;
        }
    }
    return true;
}

// 1手詰め（§E11）：王手をかける手で、相手の玉が逃げられず、取られず、合駒もできない手を探す。
// 静的な判定で候補を絞る（逃げ道が 3 つ以上あれば、1手では詰まないとみなす。王手になりうる手だけを見る）。
// 見つけた手は、自玉の安全と詰みを正確に確かめるので、詰みでない手を詰みとは返さない。
bool Worker::find_mate1(const Move* moves, int n, Move& out) const {
    const Board& b = game_.board;
    const int us = b.turn, them = 1 - us;
    const int k = b.king_sq[them];
    if (k < 0) return false;
    const int kx = k % 9, ky = k / 9;
    int flights = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const int x = kx + dx, y = ky + dy;
            if ((!dx && !dy) || x < 0 || x > 8 || y < 0 || y > 8) continue;
            const int sq = y * 9 + x;
            if (b.pieces[sq] && ((b.pieces[sq] > 16) ? WHITE : BLACK) == them) continue;
            if (!b.is_attacked(sq, us) && ++flights > 2) return false;
        }
    }
    for (int i = 0; i < n; i++) {
        const Move& m = moves[i];
        if (m.is_drop && m.type == PAWN) continue;  // 打ち歩詰めは反則なので、詰みの手にならない
        // 王手になりうる手（動いた先が王の近くか筋上、または動く前のマスが王の筋上＝開き王手）だけを見る
        if (!near_line(m.to, kx, ky) && (m.is_drop || !near_line(m.from, kx, ky))) continue;
        Position next = b.bit_pos;
        next.do_move(m);
        if (next.is_attacked(next.king_sq[us], them)) continue;   // 自玉が利きにさらされる（非合法）
        if (!next.is_attacked(next.king_sq[them], us)) continue;  // 王手になっていない
        if (!has_legal_moves(next)) {
            out = m;
            return true;
        }
    }
    return false;
}

// 静止探索：駒を取る手だけを読む。王手されているときは全合法手（§E5.1）。
// qd は静止探索に入ってからの手数。王手の応酬が続くと分岐が膨らむので、上限を超えたら評価して止める。
int Worker::qsearch(int alpha, int beta, int ply, int qd) {
    if (aborted()) return 0;
    if (ply > seldepth_) seldepth_ = ply;
    if (ply >= kMaxPly - 1) return evaluate();

    const bool in_check = game_.in_check_now();
    if ((in_check && qd > kQCheckLimit) || qd > kQLimit) return evaluate();

    // 置換表（静止探索の結果も深さ0として保存する。N4.1）
    const uint64_t qkey = game_.board.zobrist_key;
    const int qalpha0 = alpha;
    {
        TTEntry te;
        if (!sh_.no_tt_read && sh_.tt->probe(qkey, te)) {
            const int s = from_tt(te.score, ply);
            if (te.bound == kExact || (te.bound == kLower && s >= beta) || (te.bound == kUpper && s <= alpha)) return s;
        }
    }

    int best;
    MoveBuf& mb = buf_[ply];
    Move* moves = mb.moves;
    int* score = mb.score;
    int n = 0;
    if (!in_check) {
        const int stand = evaluate();
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
        best = stand;
        const int all = sh_.opt.search_lazy_legal ? (int)(game_.gen_pseudo(moves) - moves)
                                              : (int)(game_.gen_legal(moves) - moves);
        for (int i = 0; i < all; i++) {
            const Move m = moves[i];
            if (!is_capture(game_.board, m)) continue;
            const int victim = kPieceValue[game_.board.pieces[m.to] & 15];
            const int attacker = kPieceValue[game_.board.pieces[m.from] & 15];
            if (stand + victim * 3 / 2 + 200 <= alpha) continue;  // delta pruning
            if (sh_.opt.search_qsee) {
                if (see(m) < 0) continue;  // 損な取り（SEE が負）は読まない
            }
            else if (victim < attacker && game_.board.is_attacked(m.to, 1 - game_.board.turn)) {
                continue;  // N5 の簡易判定：安い駒で高い駒を取って、そのマスに取り返される手
            }
            moves[n++] = m;
        }
    }
    else {
        n = (int)(game_.gen_legal(moves) - moves);
        if (n == 0) return -(kMate - ply);
        best = -kInf;
    }

    for (int i = 0; i < n; i++) score[i] = order_score(moves[i], Move{}, false, ply, nullptr);
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) {
            if (score[j] > score[bi]) bi = j;
        }
        std::swap(moves[i], moves[bi]);
        std::swap(score[i], score[bi]);

        game_.push(moves[i]);
        // 疑似合法手は、指した後に自玉の安全を確かめる（王手を受けている局面の手は、もともと合法手）
        if (!in_check && game_.board.in_check(1 - game_.board.turn)) {
            game_.pop();
            continue;
        }
        cur_key_[ply + 1] = piece_key(moves[i]);
        const int s = -qsearch(-beta, -alpha, ply + 1, qd + 1);
        game_.pop();
        if (sh_.stop.load(std::memory_order_relaxed)) return 0;
        if (s > best) {
            best = s;
            if (s > alpha) {
                alpha = s;
                if (alpha >= beta) break;
            }
        }
    }
    sh_.tt->store(qkey, 0, to_tt(best, ply), best >= beta ? kLower : (best > qalpha0 ? kExact : kUpper), Move{}, false);
    return best;
}

int Worker::negamax(int depth, int alpha, int beta, int ply, bool allow_null) {
    if (aborted()) return 0;
    if (ply > seldepth_) seldepth_ = ply;
    pv_len_[ply] = 0;

    if (ply > 0) {
        // 千日手（連続王手を含む）と最大手数（§E2.3）
        const Repetition r = game_.repetition();
        if (r == Repetition::Draw) return draw_score();
        if (r == Repetition::Win) return kMateCp;
        if (r == Repetition::Lose) return -kMateCp;
        if (sh_.opt.max_ply > 0 && game_.ply_count() >= sh_.opt.max_ply) return draw_score();
    }
    if (depth <= 0 || ply >= kMaxPly - 1) return qsearch(alpha, beta, ply, 0);

    const Options& o = sh_.opt;
    const bool pv_node = beta - alpha > 1;
    const bool in_check = game_.in_check_now();
    const uint64_t key = game_.board.zobrist_key;

    // 置換表
    TTEntry tte;
    bool has_tt = false;
    Move tt_move{};
    if (!sh_.no_tt_read && sh_.tt->probe(key, tte)) {
        has_tt = tte.has_move;
        tt_move = tte.move;
        if (!pv_node && tte.depth >= depth) {
            const int s = from_tt(tte.score, ply);
            if (tte.bound == kExact || (tte.bound == kLower && s >= beta) || (tte.bound == kUpper && s <= alpha)) {
                return s;
            }
        }
    }

    // ヒントの表（§E5.5）
    const std::vector<HintEntry>* hint = sh_.hints ? sh_.hints->find(key) : nullptr;

    // 静的評価（王手を受けていないときだけ）。改善の判定と枝刈りに使い、各節点の値を覚えておく
    const int static_eval = in_check ? kNoEval : evaluate();
    eval_stack_[ply] = static_eval;
    const bool improving = o.search_improving && !in_check && ply >= 2 && eval_stack_[ply - 2] != kNoEval &&
                           static_eval > eval_stack_[ply - 2];
    const bool prune_ok = !pv_node && !in_check;  // 枝刈りをしてよい節点（PV でなく、王手を受けていない）

    // razoring：浅い深さで評価が α を大きく下回るなら、静止探索で確かめて返す
    if (o.search_razoring && prune_ok && depth <= 2 && static_eval + o.search_razor_margin * depth <= alpha) {
        const int v = qsearch(alpha, alpha + 1, ply, 0);
        if (sh_.stop.load(std::memory_order_relaxed)) return 0;
        if (v <= alpha) return v;
    }

    // reverse futility（RFP）：評価が β を大きく超えているなら、浅い探索を省いてそのまま返す
    if (o.search_rfp && prune_ok && depth <= o.search_rfp_depth && std::abs(beta) < kMateBound && !(o.search_rfp_guard && king_boxed()) &&
        static_eval - o.search_rfp_margin * depth + (improving ? 60 : 0) >= beta) {
        return static_eval;
    }

    // null move pruning（§E5.1）。削減量は N4.1 の 2 + depth/4、または深さと評価値の差で決める（§E11）
    if (prune_ok && allow_null && depth >= 3 && static_eval >= beta && has_non_pawn_material()) {
        const int r = o.search_null_dynamic ? 3 + depth / 3 + std::min(3, (static_eval - beta) / 200) : 2 + depth / 4;
        game_.push_null();
        cur_key_[ply + 1] = -1;
        const int s = -negamax(depth - 1 - r, -beta, -beta + 1, ply + 1, false);
        game_.pop();
        if (sh_.stop.load(std::memory_order_relaxed)) return 0;
        if (s >= beta) {
            if (!(o.search_null_dynamic && depth >= 12)) return s >= kMateBound ? beta : s;
            // 深いところでは、手を渡さずに確かめる（検証探索）。確かめられなければ普通に読む
            const int v = negamax(depth - r, beta - 1, beta, ply, false);
            if (sh_.stop.load(std::memory_order_relaxed)) return 0;
            if (v >= beta) return v >= kMateBound ? beta : v;
        }
    }

    // 内部の反復深化（IIR）：置換表に手が無ければ、手の順番の見当が付かないので1手浅く読む
    if (o.search_iir && !in_check && !has_tt && depth >= 4) depth--;

    // 指し手の生成。王手を受けているときは合法手、そうでなければ疑似合法手（指した後に確かめる）
    MoveBuf& mb = buf_[ply];
    Move* moves = mb.moves;
    int* score = mb.score;
    const int n = (in_check || !o.search_lazy_legal) ? (int)(game_.gen_legal(moves) - moves)
                                                     : (int)(game_.gen_pseudo(moves) - moves);
    if (n == 0) return -(kMate - ply);  // 手が無い＝負け（王手されていれば詰み）

    // 1手詰め（§E11）：王手で詰む手があれば、その値を返す（TT には入れない）
    if (o.search_mate1 && !in_check && depth >= 2) {
        Move mate{};
        if (find_mate1(moves, n, mate)) {
            if (pv_node) set_pv(ply, mate);
            return kMate - (ply + 1);
        }
    }

    for (int i = 0; i < n; i++) score[i] = order_score(moves[i], tt_move, has_tt, ply, hint);

    const int alpha0 = alpha;
    int best = -kInf;
    Move best_move{};
    int legal = 0;  // 合法と分かった手の数（この局面で何番目に読む手か）
    int nq = 0;     // 試した静かな手（β カットのときに減点する）
    Move quiets[kMaxQuiets];
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) {
            if (score[j] > score[bi]) bi = j;
        }
        std::swap(moves[i], moves[bi]);
        std::swap(score[i], score[bi]);
        const Move m = moves[i];
        const bool capture = is_capture(game_.board, m);
        const bool quiet = !capture && !m.is_promote;

        // 枝刈り（PV でなく王手を受けていない局面で、最初の合法手の後。最善が詰まされる値のときは読む）
        if (prune_ok && legal > 0 && best > -kMateBound) {
            if (quiet && depth <= 3 && static_eval + o.search_futility_margin * depth <= alpha) continue;  // futility（N4.1）
            if (quiet && !o.search_lmp && depth <= 3 && legal >= 6 + 3 * depth * depth) continue;  // 手の数（N4.1）
        }
        // 新しい枝刈りは、王手になりうる手には使わない（王手で詰む手を、静かな手として刈らないため）
        const int opp_k = game_.board.king_sq[1 - game_.board.turn];
        const bool maybe_check = opp_k >= 0 && (near_line(m.to, opp_k % 9, opp_k / 9) ||
                                                (!m.is_drop && near_line(m.from, opp_k % 9, opp_k / 9)));
        if (prune_ok && legal > 0 && best > -kMateBound && !maybe_check) {
            if (quiet && o.search_lmp && depth <= 8 &&
                legal >= o.search_lmp_scale * (3 + depth * depth) / (improving ? 1 : 2)) continue;  // LMP
            if (o.search_drop_prune && depth <= 2 && m.is_drop && quiet_drop_far(m)) continue;
            if (o.search_see_prune && depth <= 6) {
                // 相手の利きが無いマスへの静かな手は、SEE が 0 なので調べない
                const bool hit = capture || game_.board.is_attacked(m.to, 1 - game_.board.turn);
                if (hit && see(m) < (capture ? -100 * depth : -o.search_see_margin * depth)) continue;
            }
        }

        const int hs = (quiet && o.search_lmr_table) ? quiet_score(m, ply) : 0;  // 手を指す前に求める（手番が変わるため）
        game_.push(m);
        // 疑似合法手は、指した後に自玉の安全を確かめる。打ち歩詰めも、ここで反則として外す
        if (!in_check) {
            if (game_.board.in_check(1 - game_.board.turn)) {
                game_.pop();
                continue;
            }
            if (m.is_drop && m.type == PAWN && game_.in_check_now() && !has_reply()) {
                game_.pop();
                continue;
            }
        }
        legal++;
        cur_key_[ply + 1] = piece_key(m);
        const bool gives_check = game_.in_check_now();
        const int nd = depth - 1;

        int s;
        if (legal == 1) {
            s = -negamax(nd, -beta, -alpha, ply + 1, true);
        }
        else {
            // LMR（§E5.1）：後ろの静かな手は浅く読む。王手・王手を受けている局面・ヒントの確率が高い手は減らさない（§E5.5）
            int red = 0;
            if (depth >= 3 && quiet && !in_check && !gives_check) {
                if (o.search_lmr_table) {
                    red = lmr_table(depth, legal);
                    if (!o.search_improving || !improving) red++;
                    if (pv_node) red--;
                    red -= std::clamp(hs / 8192, -2, 2);  // 履歴の良い手は減らし、悪い手は増やす
                }
                else if (legal >= 4) {  // N5 の決まり：0 から数えて 3 番目以降の静かな手（legal は 1 から数える）
                    red = legal >= 7 ? 2 : 1;
                }
                if (red > 0 && hint) {
                    for (const HintEntry& h : *hint) {
                        if (same_move(h.move, m) && h.prob >= o.hint_lmr_threshold) red = 0;
                    }
                }
                red = std::clamp(red, 0, std::max(0, nd - 1));
            }
            s = -negamax(nd - red, -alpha - 1, -alpha, ply + 1, true);
            if (red > 0 && s > alpha) s = -negamax(nd, -alpha - 1, -alpha, ply + 1, true);
            if (pv_node && s > alpha && s < beta) s = -negamax(nd, -beta, -alpha, ply + 1, true);
        }
        game_.pop();
        if (sh_.stop.load(std::memory_order_relaxed)) return 0;

        if (s > best) {
            best = s;
            best_move = m;
            if (s > alpha) {
                alpha = s;
                if (pv_node) set_pv(ply, m);
                if (alpha >= beta) {
                    if (!capture) record_quiet_cutoff(m, depth, ply, quiets, nq);
                    break;
                }
            }
        }
        if (quiet && nq < kMaxQuiets) quiets[nq++] = m;
    }
    if (legal == 0) return -(kMate - ply);  // 合法手が無い（王手されていれば詰み。N4 の決まり）

    const int bound = best >= beta ? kLower : (best > alpha0 ? kExact : kUpper);
    sh_.tt->store(key, depth, to_tt(best, ply), bound, best_move, true);
    return best;
}

// 根の探索（§E5.4）。rm[pv_idx..] を探す。rm の記録を更新する
int Worker::search_root(int depth, int alpha, int beta, int pv_idx, std::vector<RootMove>& rm) {
    const bool in_check = game_.board.in_check(game_.board.turn);
    int best = -kInf;
    for (size_t i = pv_idx; i < rm.size(); i++) {
        const Move m = rm[i].move;
        const long long nodes0 = total_nodes();
        game_.push(m);
        cur_key_[1] = piece_key(m);
        const int ext = in_check ? 1 : 0;  // 根だけ：王手を受けている局面の手は1手延長する（N4.1）
        const int nd = depth - 1 + ext;

        int s;
        if ((int)i == pv_idx) {
            s = -negamax(nd, -beta, -alpha, 1, true);
        }
        else {
            s = -negamax(nd, -alpha - 1, -alpha, 1, true);
            if (s > alpha && !sh_.stop.load(std::memory_order_relaxed)) {
                s = -negamax(nd, -beta, -alpha, 1, true);
            }
        }
        game_.pop();
        if (sh_.stop.load(std::memory_order_relaxed)) return 0;
        rm[i].nodes += total_nodes() - nodes0;

        if ((int)i == pv_idx || s > alpha) {
            rm[i].score = s;
            rm[i].depth = depth;
            rm[i].seldepth = seldepth_;
            rm[i].bound = s >= beta ? kLower : (s <= alpha ? kUpper : kExact);
            rm[i].pv.assign(1, m);
            for (int k = 0; k < pv_len_[1]; k++) rm[i].pv.push_back(pv_at(1, k));
        }
        else if (!(sh_.exact_all && rm[i].depth == depth && rm[i].bound == kExact)) {
            // 窓に届かなかった手：値は上限（この手はこれ以下）。読み筋は前の段のものを残す。
            // exact_all では、この段で正確な値を得た手は上書きしない
            rm[i].score = s;
            rm[i].bound = kUpper;
        }
        if (s > best) best = s;
        if (s > alpha) alpha = s;
        if (alpha >= beta) break;
    }
    return best;
}

}  // namespace

// 反復深化の本体。main（on_depth あり）は結果を報告し、helper は報告せずに探索を進める（Lazy SMP）。
template <class F>
void Worker::iterate(std::vector<RootMove>& rm, int max_depth, const F* on_depth) {
    const int multipv = std::max(1, std::min<int>(sh_.opt.multipv, (int)rm.size()));
    for (int depth = 1; depth <= std::min(max_depth, sh_.max_depth.load()); depth++) {
        // helper は段を飛ばして、main と違う順で読む（Lazy SMP）
        if (on_depth == nullptr && id_ % 2 == 1 && depth % 2 == 0 && depth > 2) continue;

        seldepth_ = 0;
        bool aborted_now = false;
        int pv = 0;
        int repass = 0;  // exact_all：同じ位置を読み直した回数（この段の中で数える）
        while (pv < multipv) {
            const int prev = rm[pv].score;
            int delta = 20;
            int alpha = -kInf, beta = kInf;
            if (depth >= 4) {
                alpha = std::max(-kInf, prev - delta);
                beta = std::min(kInf, prev + delta);
            }
            for (;;) {
                search_root(depth, alpha, beta, pv, rm);
                if (sh_.stop.load(std::memory_order_relaxed)) break;
                // aspiration window の失敗：窓を広げて読み直す
                if (alpha > -kInf && rm[pv].score <= alpha) {
                    delta *= 2;
                    alpha = delta > 500 ? -kInf : std::max(-kInf, alpha - delta);
                    continue;
                }
                if (beta < kInf && rm[pv].score >= beta) {
                    delta *= 2;
                    beta = delta > 500 ? kInf : std::min(kInf, beta + delta);
                    continue;
                }
                break;
            }
            if (sh_.stop.load(std::memory_order_relaxed)) {
                aborted_now = true;
                break;
            }
            std::stable_sort(rm.begin() + pv, rm.end(),
                             [](const RootMove& a, const RootMove& b) { return a.score > b.score; });
            // exact_all（rank）：並べ替えの後に同じ位置へ来た手が、この段の正確な値でなければ、その手をもう一度読む
            if (sh_.exact_all && !(rm[pv].depth == depth && rm[pv].bound == kExact) && repass++ < 4 * multipv) continue;
            pv++;
            // exact_all：この段で正確な値を得た手は読み飛ばす（並べ替えは、読んだ位置より後ろしか動かさない）
            if (sh_.exact_all) {
                while (pv < multipv && rm[pv].depth == depth && rm[pv].bound == kExact) pv++;
            }
        }
        if (aborted_now) break;
        sh_.nodes.fetch_add(pending_, std::memory_order_relaxed);  // 報告の前に、ノード数を共有へ足す
        pending_ = 0;
        if (on_depth) (*on_depth)(depth, rm, seldepth_);
    }
    sh_.nodes.fetch_add(pending_, std::memory_order_relaxed);
    pending_ = 0;
}

// ---------- ジョブ ----------

namespace {

constexpr int kDjpMateTableSize = 1 << 18;      // DJP の詰み探索の表（ジョブごと）
constexpr long long kDjpMateNodes = 100000000;  // 制限の無い詰み探索のノード数の上限

// helper の作業の締め切り。本体が終わると、始まった helper だけを待つ。まだ始まっていない作業は何もせずに戻る
struct HelperSync {
    std::mutex mu;
    std::condition_variable cv;
    int started = 0;
    int finished = 0;
    bool closed = false;

    bool enter() {
        std::lock_guard<std::mutex> lk(mu);
        if (closed) return false;
        started++;
        return true;
    }
    void leave() {
        {
            std::lock_guard<std::mutex> lk(mu);
            finished++;
        }
        cv.notify_all();
    }
    void close() {
        std::unique_lock<std::mutex> lk(mu);
        closed = true;
        cv.wait(lk, [this] { return finished == started; });
    }
};

// 探索の深さごとの報告（§E5.8 の出来事を含む）。info 行は sink へ、根の手ごとの値は root_report へ渡す
struct Reporter {
    Job::Shared* sh;
    long long start_ms;
    int multipv;
    mutable Move prev_best{};
    mutable bool has_prev = false;
    mutable int prev_score = 0;
    mutable bool mate_reported = false;
    std::vector<RootMove>* out;

    void operator()(int depth, const std::vector<RootMove>& rm, int seldepth) const {
        const long long nodes = sh->nodes.load(std::memory_order_relaxed);
        const long long ms = std::max<long long>(1, now_ms() - start_ms);
        const int lines = std::min<int>(multipv, (int)rm.size());
        for (int k = 0; k < lines; k++) {
            const RootMove& r = rm[k];
            std::string line = "info depth " + std::to_string(depth) + " seldepth " + std::to_string(std::max(seldepth, r.seldepth));
            if (multipv > 1) line += " multipv " + std::to_string(k + 1);
            line += " score " + score_text(r.score);
            line += " nodes " + std::to_string(nodes) + " nps " + std::to_string(nodes * 1000 / ms);
            line += " time " + std::to_string(ms);
            line += " hashfull " + std::to_string(sh->tt->hashfull());
            line += " pv";
            for (const Move& m : r.pv) line += " " + move_to_usi(m);
            if (sh->sink) sh->sink(line);
        }
        // 出来事の通知（§E5.8）：最善手の交代、評価の急変、詰みの発見
        if (sh->sink && !rm.empty()) {
            const RootMove& best = rm[0];
            if (!has_prev || !(best.move.from == prev_best.from && best.move.to == prev_best.to &&
                               best.move.type == prev_best.type && best.move.is_drop == prev_best.is_drop &&
                               best.move.is_promote == prev_best.is_promote)) {
                if (has_prev) sh->sink("info string event best_move_changed " + move_to_usi(best.move));
            }
            if (has_prev && std::abs(best.score - prev_score) >= 150 && std::abs(prev_score) < kMateBound) {
                sh->sink("info string event score_swing " + std::to_string(best.score - prev_score));
            }
            if (!mate_reported && best.score >= kMateBound) {
                mate_reported = true;
                sh->sink("info string event mate_found " + score_text(best.score));
            }
            if (!mate_reported && best.score <= -kMateBound) {
                mate_reported = true;
                sh->sink("info string event mated_found " + score_text(best.score));
            }
            prev_best = best.move;
            has_prev = true;
            prev_score = best.score;
        }
        if (sh->root_report) sh->root_report(depth, rm, nodes, ms);
        *out = rm;
    }
};

}  // namespace

Job::Job(const Game& root, const Limits& lim, const Options& opt, const EvalRef& ev, TranspositionTable& tt,
         ThreadPool* pool, const HintTable* hints, bool null_root, InfoSink sink)
    : root_(root), lim_(lim), pool_(pool), null_root_(null_root) {
    sh_.tt = &tt;
    sh_.hints = opt.hint_weight > 0 ? hints : nullptr;  // hint_weight が 0 のときは、ヒントを使わない（§E5.5）
    sh_.opt = opt;
    sh_.ev = ev;
    sh_.sink = std::move(sink);
    sh_.node_limit = lim.nodes;
    sh_.exact_all = lim.exact_all;
    sh_.no_tt_read = lim.mode == JobMode::kProbe;
    sh_.max_depth = lim.depth > 0 ? std::min(lim.depth, kMaxDepth) : kMaxDepth;
}

void Job::stop() {
    searching_ = false;  // 根の詰み探索も、この合図で止まる
    sh_.stop = true;
}

long long Job::nodes() const { return sh_.nodes.load(); }

// 動いているジョブに上限を付ける（§E9.5 の promote）。時間は今から、ノード数は今の数から足す
void Job::extend(long long time_ms, long long nodes, int depth) {
    if (time_ms > 0) sh_.deadline_ms = now_ms() + time_ms;
    if (nodes > 0) sh_.node_limit = sh_.nodes.load() + nodes;
    if (depth > 0) sh_.max_depth = std::min(depth, kMaxDepth);
}

// 持ち時間から思考時間を求める（§E5.1）。秒読みは次の手の持ち時間として扱い、NetworkDelay を差し引く
long long Job::budget_ms() const {
    if (lim_.movetime > 0) return std::max<long long>(1, lim_.movetime - sh_.opt.network_delay);
    const bool black = root_.board.turn == BLACK;
    const long long my = std::max<long long>(0, black ? lim_.btime : lim_.wtime);
    const long long inc = black ? lim_.binc : lim_.winc;
    const bool has_clock = lim_.btime >= 0 || lim_.wtime >= 0 || lim_.byoyomi > 0;
    if (!has_clock) return -1;
    const long long margin = 100;
    long long think = my / 25 + inc * 3 / 4;
    if (lim_.byoyomi > 0) {
        think += lim_.byoyomi - margin;
        think = std::min(think, my + lim_.byoyomi - margin);
    }
    else {
        think = std::min(think, my - margin);
    }
    think -= sh_.opt.network_delay;
    return std::max<long long>(1, think);
}

void Job::ponder_hit() {
    const long long b = budget_ms();
    sh_.deadline_ms = b > 0 ? now_ms() + b : 0;
}

// kProbe（§E5.4、§E9.4）：窓 [T−1, T] で深さ順に読む。読み切った段の結論を、最後の結論とする
void Job::run_probe(Game& game, std::vector<RootMove>& rm) {
    const int t = lim_.probe_threshold;
    Worker w(sh_, game, 0);
    int done = 0;
    int score = 0;
    const int max_depth = sh_.max_depth.load();
    for (int d = 1; d <= max_depth; d++) {
        std::vector<RootMove> one = rm;
        w.search_root(d, t - 1, t, 0, one);
        if (sh_.stop.load(std::memory_order_relaxed)) break;
        if (lim_.nodes > 0 && w.total_nodes() > lim_.nodes) break;  // 上限を超えて読み切った段は数えない
        done = d;
        score = one[0].score;
    }
    sh_.nodes.fetch_add(w.pending_, std::memory_order_relaxed);  // 数えていない節点を足す
    w.pending_ = 0;
    ProbeOut& p = probe_;
    p.threshold = t;
    p.depth = done;
    p.score = score;
    if (done > 0 && done >= lim_.probe_mindepth) {
        p.verdict = score >= t ? 1 : 0;
        // 咎める手順：この手を指した後の局面から、置換表の最善の応手を順にたどる（段は done まで）。
        // 読み直しはしない（ゼロ幅の窓では読み筋が残らず、全幅で読み直すと予算を超えるため）
        if (p.verdict == 0) {
            Game after = game;
            after.push(rm[0].move);
            for (int i = 0; i < done; i++) {
                TTEntry te;
                if (!sh_.tt->probe(after.board.zobrist_key, te) || !te.has_move) break;
                const std::string u = move_to_usi(te.move);
                const std::vector<Move> legal = after.legal_moves();
                auto it = std::find_if(legal.begin(), legal.end(), [&u](const Move& m) { return move_to_usi(m) == u; });
                if (it == legal.end()) break;
                p.pv.push_back(*it);
                after.push(*it);
            }
        }
    }
    result_.assign(1, rm[0]);
    result_[0].score = score;
    result_[0].depth = done;
    result_[0].bound = p.verdict == 1 ? kLower : kUpper;
}

// kMate（§E5.7）：df-pn で詰みを探す。見つからなかったときは unknown（否定は証明しない）
void Job::run_mate(const Game& game) {
    DfpnSearcher mate(&searching_, kDjpMateTableSize);
    mate.set_max_nodes(lim_.nodes > 0 ? lim_.nodes : kDjpMateNodes);
    const long long dl = sh_.deadline_ms.load();
    if (dl > 0) mate.set_deadline_ms(dl);
    if (!mate.search_mate(game.board)) return;
    mate_pv_ = mate.get_mate_pv(game.board);
    mate_found_ = !mate_pv_.empty();
    if (mate_found_) best_move_ = mate_pv_[0];
}

void Job::run() {
    const long long start = now_ms();
    // 駆動スレッドは、cpus の先頭の論理CPUに固定する（§E2.4。ヘルパーは残りの CPU）
    if (!sh_.opt.cpus.empty()) {
        std::string err;
        if (!cpu::pin_current_thread(sh_.opt.cpus[0], err) && sh_.sink) {
            sh_.sink("info string Nuenagon cpu: 探索スレッドを固定できない（" + err + "）。OS に任せる");
        }
    }
    if (!lim_.ponder) {
        const long long b = budget_ms();
        sh_.deadline_ms = b > 0 ? start + b : 0;
    }

    Game game = root_;
    if (null_root_) {
        if (game.board.in_check(game.board.turn)) {
            best_move_ = "resign";
            return;
        }
        game.push_null();
    }
    if (game.board.can_declare_win()) {
        best_move_ = "win";
        return;
    }
    if (lim_.mode == JobMode::kMate) {
        run_mate(game);
        return;
    }

    // 根の手（searchmoves で絞り、exclude で除く。§E5.4）
    std::vector<RootMove> rm;
    for (const Move& m : game.legal_moves()) {
        const std::string u = move_to_usi(m);
        if (!lim_.searchmoves.empty() && std::find(lim_.searchmoves.begin(), lim_.searchmoves.end(), u) == lim_.searchmoves.end()) {
            continue;
        }
        if (std::find(lim_.excludemoves.begin(), lim_.excludemoves.end(), u) != lim_.excludemoves.end()) continue;
        RootMove r;
        r.move = m;
        rm.push_back(r);
    }
    if (rm.empty() && !lim_.searchmoves.empty()) {
        // 絞った結果が空なら、合法手すべてを読む（USI の決まり）
        for (const Move& m : game.legal_moves()) {
            RootMove r;
            r.move = m;
            rm.push_back(r);
        }
    }
    if (rm.empty()) {
        best_move_ = "resign";
        return;
    }
    best_move_ = move_to_usi(rm[0].move);
    if (lim_.mode == JobMode::kProbe) {
        run_probe(game, rm);
        return;
    }

    // 根の詰み探索（§E5.7）：短い時間（ノード数）だけ詰みを探し、見つかればその手で終える
    if (!null_root_ && !lim_.ponder && lim_.root_mate_probe && (sh_.ev.net || sh_.ev.hk)) {
        DfpnSearcher mate(&searching_, kMateProbeTableSize);  // 表はこの探索専用（共有の変数は使わない）
        mate.set_max_nodes(kMateProbeNodes);
        // 詰み探索は持ち時間の1割まで（締め切りが無い探索では 200ms まで）。決定的モードでは時間で止めない（§E5.9）
        if (!sh_.opt.deterministic) {
            const long long now = now_ms();
            const long long dl = sh_.deadline_ms.load();
            mate.set_deadline_ms(now + (dl > 0 ? std::max<long long>(1, (dl - now) / 10) : kMateProbeMsNoLimit));
        }
        if (mate.search_mate(game.board)) {
            const std::string usi = mate.get_best_mate();
            for (const RootMove& r : rm) {
                if (move_to_usi(r.move) != usi) continue;
                const int plies = std::max(1, mate.get_mate_length());
                RootMove found = r;
                found.score = kMate - plies;
                found.depth = plies;
                found.seldepth = plies;
                found.pv.assign(1, r.move);
                result_.assign(1, found);
                best_move_ = usi;
                if (sh_.sink) {
                    sh_.sink("info depth " + std::to_string(plies) + " seldepth " + std::to_string(plies) +
                             " score mate " + std::to_string(plies) + " nodes 0 nps 0 time " +
                             std::to_string(std::max<long long>(1, now_ms() - start)) + " pv " + usi);
                }
                if (sh_.root_report) {
                    sh_.root_report(plies, result_, sh_.nodes.load(), std::max<long long>(1, now_ms() - start));
                }
                return;
            }
        }
    }

    const int max_depth = lim_.depth > 0 ? std::min(lim_.depth, kMaxDepth) : kMaxDepth;
    const int threads = sh_.opt.threads;
    int helpers = 0;
    if (!sh_.opt.deterministic && pool_) helpers = std::max(0, std::min(threads - 1, pool_->size()));

    // helper（Lazy SMP）：置換表を共有して、段を変えて読む
    auto helpers_sync = std::make_shared<HelperSync>();
    for (int h = 1; h <= helpers; h++) {
        pool_->submit([this, h, helpers_sync, &game, max_depth, rm]() mutable {
            if (!helpers_sync->enter()) return;
            Worker w(sh_, game, h);
            w.iterate(rm, max_depth, static_cast<const Reporter*>(nullptr));
            helpers_sync->leave();
        });
    }

    Reporter report{&sh_, start, sh_.opt.multipv, Move{}, false, 0, false, &result_};
    Worker main_worker(sh_, game, 0);
    std::vector<RootMove> main_rm = rm;
    main_worker.iterate(main_rm, max_depth, &report);

    // 止めて、始まった helper が終わるまで待つ
    sh_.stop = true;
    helpers_sync->close();

    // bestmove の直前の info（§E7、N4.1）：最後に読み終えた深さの結果に、止めた時点の合計ノード数と時間を付けて出す
    if (!result_.empty() && sh_.sink) {
        const long long nodes = sh_.nodes.load();
        const long long ms = std::max<long long>(1, now_ms() - start);
        const int lines = std::min<int>(std::max(1, sh_.opt.multipv), (int)result_.size());
        for (int k = 0; k < lines; k++) {
            const RootMove& r = result_[k];
            std::string line = "info depth " + std::to_string(std::max(1, r.depth)) + " seldepth " +
                               std::to_string(std::max(r.depth, r.seldepth));
            if (sh_.opt.multipv > 1) line += " multipv " + std::to_string(k + 1);
            line += " score " + score_text(r.score) + " nodes " + std::to_string(nodes) +
                    " nps " + std::to_string(nodes * 1000 / ms) + " time " + std::to_string(ms) +
                    " hashfull " + std::to_string(sh_.tt->hashfull()) + " pv";
            for (const Move& m : r.pv) line += " " + move_to_usi(m);
            sh_.sink(line);
        }
    }
    if (!result_.empty()) {
        best_move_ = move_to_usi(result_[0].move);
        if (sh_.opt.usi_ponder && result_[0].pv.size() >= 2) {
            ponder_move_ = move_to_usi(result_[0].pv[1]);
        }
    }
}

int qsearch_score(const Game& g, const EvalRef& ev) {
    TranspositionTable tt;
    tt.resize(1);
    Job::Shared sh;
    sh.tt = &tt;
    sh.ev = ev;
    Worker w(sh, g, 0);
    return w.qsearch(-kInf, kInf, 0, 0);
}

}  // namespace search
