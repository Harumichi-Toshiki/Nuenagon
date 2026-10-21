// αβ探索の実装（設計書 §E5）。反復深化、aspiration window、PVS、置換表、null move、LMR、王手延長、
// 静止探索、指し手の並べ替え、千日手・最大手数・詰み、時間管理、Lazy SMP。
#include "search/search.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

#include "eval/raypair/raypair.h"
#include "mate/dfpn.h"

namespace search {

// ---------- 小さな道具 ----------

namespace {

using Clock = std::chrono::steady_clock;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

constexpr int kCheckInterval = 1024;     // 時間・ノード数・検査は、この間隔のノードごとに行う
constexpr int kHistSize = 89;            // 履歴の添字：盤上の手は from（0..80）、駒打ちは 81 + 駒種
constexpr long long kMateProbeNodes = 300000;  // 根の詰み探索に使うノード数の上限
constexpr int kMateProbeTableSize = 1 << 16;   // 根の詰み探索の表の大きさ（大きい表は確保だけで数十ms かかる）
constexpr long long kMateProbeMsNoLimit = 200; // 締め切りの無い探索での詰み探索の時間（ms）
constexpr int kQLimit = 16;              // 静止探索の手数の上限（駒を取る手だけ）
constexpr int kQCheckLimit = 4;          // 静止探索で王手を受けている局面の手数の上限

// 駒の価値（並べ替え用。駒種 0..15。成駒は元の駒＋少し）
constexpr int kPieceValue[16] = {0, 100, 300, 350, 450, 500, 800, 900, 10000, 420, 420, 420, 420, 0, 1000, 1100};

bool same_move(const Move& a, const Move& b) {
    return a.from == b.from && a.to == b.to && a.type == b.type && a.is_promote == b.is_promote && a.is_drop == b.is_drop;
}

bool is_capture(const Board& b, const Move& m) { return !m.is_drop && b.pieces[m.to] != 0; }

int hist_index(const Move& m) { return m.is_drop ? 81 + m.type : m.from; }

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

ThreadPool::ThreadPool(int n) {
    for (int i = 0; i < n; i++) workers_.emplace_back([this] { loop(); });
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

void ThreadPool::loop() {
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
        : sh_(sh), game_(root), id_(id), history_(2 * kHistSize * 81, 0), buf_(new MoveBuf[kMaxPly + 1]) {
        std::memset(killers_, 0, sizeof killers_);
        std::memset(pv_len_, 0, sizeof pv_len_);
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
    int order_score(const Move& m, const Move& tt_move, bool has_tt, int ply, const std::vector<HintEntry>* hint) const;
    void record_quiet_cutoff(const Move& m, int depth, int ply);
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
    Move killers_[kMaxPly][2];
    std::vector<int> history_;       // [手番][履歴の添字][到達マス]
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
    if (sh_.opt.debug_check && sh_.ev.net && !game_.accumulator_matches_full()) {
        if (sh_.sink) {
            sh_.sink("info string accumulator mismatch " + game_.board.to_sfen_key() + " " + game_.last_move_usi());
        }
        sh_.stop = true;
        return true;
    }
    return false;
}

int Worker::evaluate() const {
    const int32_t raw = raypair::forward_raw(*sh_.ev.net, game_.accumulator(), game_.board.turn);
    long long cp = raypair::cp_of(raw, sh_.ev.scale);
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

// 並べ替えの点数：置換表の手 > 駒を取る手（MVV-LVA）> ヒント > キラー > 履歴
int Worker::order_score(const Move& m, const Move& tt_move, bool has_tt, int ply, const std::vector<HintEntry>* hint) const {
    if (has_tt && same_move(m, tt_move)) return 1 << 30;
    const Board& b = game_.board;
    if (is_capture(b, m)) {
        const int victim = kPieceValue[b.pieces[m.to] & 15];
        const int attacker = kPieceValue[b.pieces[m.from] & 15];
        return (1 << 28) + victim * 8 - attacker / 8;
    }
    if (m.is_drop == false && m.is_promote) return (1 << 27) + 1;
    if (hint) {
        for (const HintEntry& h : *hint) {
            if (same_move(h.move, m)) return (1 << 26) + (int)(h.prob * 100000);
        }
    }
    if (same_move(killers_[ply][0], m) || same_move(killers_[ply][1], m)) return 1 << 25;
    return history_[(b.turn * kHistSize + hist_index(m)) * 81 + m.to];
}

void Worker::record_quiet_cutoff(const Move& m, int depth, int ply) {
    if (!same_move(killers_[ply][0], m)) {
        killers_[ply][1] = killers_[ply][0];
        killers_[ply][0] = m;
    }
    int& h = history_[(game_.board.turn * kHistSize + hist_index(m)) * 81 + m.to];
    h = std::min(h + depth * depth, 1 << 20);
}

void Worker::set_pv(int ply, const Move& m) {
    pv_[ply * kMaxPly] = m;
    const int child = pv_len_[ply + 1];
    for (int i = 0; i < child; i++) pv_[ply * kMaxPly + 1 + i] = pv_[(ply + 1) * kMaxPly + i];
    pv_len_[ply] = 1 + child;
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
        if (sh_.tt->probe(qkey, te)) {
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
        const int all = (int)(game_.gen_legal(moves) - moves);
        for (int i = 0; i < all; i++) {
            const Move m = moves[i];
            if (!is_capture(game_.board, m)) continue;
            const int victim = kPieceValue[game_.board.pieces[m.to] & 15];
            const int attacker = kPieceValue[game_.board.pieces[m.from] & 15];
            // 損な取り（安い駒を高い駒で取って、そのマスに取り返される）は読まない
            if (victim < attacker && game_.board.is_attacked(m.to, 1 - game_.board.turn)) continue;
            if (stand + victim * 3 / 2 + 200 <= alpha) continue;  // delta pruning
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

    const bool pv_node = beta - alpha > 1;
    const bool in_check = game_.in_check_now();
    const uint64_t key = game_.board.zobrist_key;

    // 置換表
    TTEntry tte;
    bool has_tt = false;
    Move tt_move{};
    if (sh_.tt->probe(key, tte)) {
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

    // null move pruning（§E5.1）
    if (!pv_node && !in_check && allow_null && depth >= 3 && has_non_pawn_material() && evaluate() >= beta) {
        const int r = 2 + depth / 4;
        game_.push_null();
        const int s = -negamax(depth - 1 - r, -beta, -beta + 1, ply + 1, false);
        game_.pop();
        if (sh_.stop.load(std::memory_order_relaxed)) return 0;
        if (s >= beta) return s >= kMateBound ? beta : s;
    }

    MoveBuf& mb = buf_[ply];
    Move* moves = mb.moves;
    int* score = mb.score;
    const int n = (int)(game_.gen_legal(moves) - moves);
    if (n == 0) return -(kMate - ply);  // 手が無い＝負け（王手されていれば詰み）

    for (int i = 0; i < n; i++) score[i] = order_score(moves[i], tt_move, has_tt, ply, hint);

    const int alpha0 = alpha;
    int best = -kInf;
    Move best_move{};
    const int static_eval = (!pv_node && !in_check && depth <= 3) ? evaluate() : 0;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) {
            if (score[j] > score[bi]) bi = j;
        }
        std::swap(moves[i], moves[bi]);
        std::swap(score[i], score[bi]);
        const Move m = moves[i];
        const bool capture = is_capture(game_.board, m);
        // 浅い深さの枝刈り（N4.1）：PV でなく王手を受けていない局面の、静かな手だけ。最善が詰まされる値のときは読む
        if (!pv_node && !in_check && depth <= 3 && i > 0 && !capture && !m.is_promote && best > -kMateBound) {
            if (static_eval + 250 * depth <= alpha) continue;                  // futility
            if ((int)i >= 6 + 3 * depth * depth) continue;                     // move count
        }

        game_.push(m);
        const bool gives_check = game_.in_check_now();
        // 王手の延長はしない（N4.1）：持ち駒の多い局面では王手の手が多く、延長すると木が膨らむ
        const int nd = depth - 1;

        int s;
        if (i == 0) {
            s = -negamax(nd, -beta, -alpha, ply + 1, true);
        }
        else {
            // LMR（§E5.1）：後ろの静かな手は浅く読む。ヒントの確率が高い手は減らさない（§E5.5）
            int red = 0;
            if (depth >= 3 && i >= 3 && !capture && !in_check && !gives_check && !m.is_promote) {
                red = i >= 6 ? 2 : 1;
                bool hinted = false;
                if (hint) {
                    for (const HintEntry& h : *hint) {
                        if (same_move(h.move, m) && h.prob >= 0.10) hinted = true;
                    }
                }
                if (hinted) red = 0;
                red = std::min(red, std::max(0, nd - 1));
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
                    if (!capture) record_quiet_cutoff(m, depth, ply);
                    break;
                }
            }
        }
    }

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
        else {
            // 窓に届かなかった手：値は上限（この手はこれ以下）。読み筋は前の段のものを残す
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
    for (int depth = 1; depth <= max_depth; depth++) {
        // helper は段を飛ばして、main と違う順で読む（Lazy SMP）
        if (on_depth == nullptr && id_ % 2 == 1 && depth % 2 == 0 && depth > 2) continue;

        seldepth_ = 0;
        bool aborted_now = false;
        for (int pv = 0; pv < multipv; pv++) {
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

// 探索の深さごとの報告（§E5.8 の出来事を含む）
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
        *out = rm;
    }
};

}  // namespace

Job::Job(const Game& root, const Limits& lim, const Options& opt, const EvalRef& ev, TranspositionTable& tt,
         ThreadPool* pool, const HintTable* hints, bool null_root, InfoSink sink)
    : root_(root), lim_(lim), pool_(pool), null_root_(null_root) {
    sh_.tt = &tt;
    sh_.hints = hints;
    sh_.opt = opt;
    sh_.ev = ev;
    sh_.sink = std::move(sink);
    sh_.node_limit = lim.nodes;
}

void Job::stop() { sh_.stop = true; }

long long Job::nodes() const { return sh_.nodes.load(); }

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

void Job::run() {
    const long long start = now_ms();
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

    // 根の手（searchmoves で絞る。§E5.4）
    std::vector<RootMove> rm;
    for (const Move& m : game.legal_moves()) {
        if (!lim_.searchmoves.empty()) {
            const std::string u = move_to_usi(m);
            if (std::find(lim_.searchmoves.begin(), lim_.searchmoves.end(), u) == lim_.searchmoves.end()) continue;
        }
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

    // 根の詰み探索（§E5.7）：短い時間（ノード数）だけ詰みを探し、見つかればその手で終える
    if (!null_root_ && !lim_.ponder && sh_.ev.net) {
        std::atomic<bool> flag{true};
        DfpnSearcher mate(&flag, kMateProbeTableSize);  // 表はこの探索専用（共有の変数は使わない）
        mate.set_max_nodes(kMateProbeNodes);
        // 詰み探索は持ち時間の1割まで（締め切りが無い探索では 200ms まで）
        const long long now = now_ms();
        const long long dl = sh_.deadline_ms.load();
        mate.set_deadline_ms(now + (dl > 0 ? std::max<long long>(1, (dl - now) / 10) : kMateProbeMsNoLimit));
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
                return;
            }
        }
    }

    const int max_depth = lim_.depth > 0 ? std::min(lim_.depth, kMaxDepth) : kMaxDepth;
    const int threads = sh_.opt.threads;
    int helpers = 0;
    if (!sh_.opt.deterministic && pool_) helpers = std::max(0, std::min(threads - 1, pool_->size()));

    // helper（Lazy SMP）：置換表を共有して、段を変えて読む
    std::atomic<int> helpers_left{helpers};
    for (int h = 1; h <= helpers; h++) {
        pool_->submit([this, h, &helpers_left, &game, max_depth, rm]() mutable {
            Worker w(sh_, game, h);
            w.iterate(rm, max_depth, static_cast<const Reporter*>(nullptr));
            helpers_left--;
        });
    }

    Reporter report{&sh_, start, sh_.opt.multipv, Move{}, false, 0, false, &result_};
    Worker main_worker(sh_, game, 0);
    std::vector<RootMove> main_rm = rm;
    main_worker.iterate(main_rm, max_depth, &report);

    // 止めて、helper が終わるまで待つ
    sh_.stop = true;
    while (helpers_left.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

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

}  // namespace search
