// αβ探索（設計書 §E5）。ジョブ（§E5.2）、スレッドプール、置換表の共有と世代（§E5.3）、
// ルートの手ごとの記録（§E5.4）、ヒントの表（§E5.5）、パスの起点（§E5.6）、出来事の通知（§E5.8）。
// 重み（Network）は評価に使うだけで、手の進め・戻しは Game（差分更新つき）を使う。
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "board/game.h"
#include "eval/halfkp/halfkp.h"
#include "eval/raypair/network.h"

namespace search {

constexpr int kMaxPly = 128;             // 探索の最大の深さ（配列の大きさ）
constexpr int kMaxDepth = 60;            // 反復深化の上限
constexpr int kInf = 32000;
constexpr int kMate = 31000;             // 詰みの値（内部の単位。手番側から見た値。詰みまでの手数で減る）
constexpr int kMateCp = 29000;           // 評価値（cp）の上限。詰みの値と重ならないように（§E4）
constexpr int kMateBound = kMate - kMaxPly;  // これ以上（以下）なら詰みの評価

enum Bound : int { kExact = 0, kLower = 1, kUpper = 2 };

// ジョブの種類（§E9.4）。search は通常の探索、probe は窓 [T−1, T] の検証、mate は詰み探索
enum class JobMode { kSearch, kProbe, kMate };

// USI の go の引数（§E2.1）。DJP のジョブも同じ形で制限を持つ
struct Limits {
    long long btime = -1, wtime = -1, binc = 0, winc = 0, byoyomi = 0, movetime = 0;
    long long nodes = 0;
    int depth = 0;
    bool infinite = false;
    bool ponder = false;                   // go ponder：ponderhit まで時間を計らない（§E2.2）
    std::vector<std::string> searchmoves;  // 読む手（空なら全部）
    std::vector<std::string> excludemoves; // 読まない手（DJP の exclude）
    JobMode mode = JobMode::kSearch;
    bool root_mate_probe = true;           // 根の詰み探索（短い時間の詰みの発見）を使うか。rank・probe は使わない
    int probe_threshold = 0;               // kProbe：閾値 T（cp。手番側から見た値）
    int probe_mindepth = 4;                // kProbe：この深さ未満の結論は unknown（§E9.4）
    bool exact_all = false;                // rank：根の手すべての値を、同じ段で正確にする（MultiPV の k を手の数とした値）
};

// 設定（USI オプション §E2.3）
struct Options {
    int threads = 4;
    int multipv = 1;
    int hash_mb = 256;
    int max_ply = 0;
    int draw_value = 0;
    int network_delay = 120;
    bool deterministic = false;   // §E5.9：1スレッド・ノード数制限
    bool debug_check = false;     // §E3.4
    bool usi_ponder = false;
    std::string cpu_affinity = "auto";  // CpuAffinity（"auto" または論理CPUの一覧。§E2.4）
    bool physical_only = false;         // UsePhysicalCoresOnly（§E2.4）
    std::vector<int> cpus;              // 探索スレッドを固定する論理CPU（空なら固定しない。main が決めて渡す）

    // 探索の改良の入り切り（§E11。USI の check "Search…"。既定は true）
    bool search_lmr_table = true;       // SearchLMRTable：LMR の削減量を表（深さ・手の順番）から決め、履歴・改善で増減する
    bool search_rfp = true;             // SearchRFP：評価が β を大きく超えたら、浅い深さで即座に返す
    bool search_null_dynamic = true;    // SearchNullMoveDynamic：null move の削減量を深さと評価値の差で決め、深いところは検証する
    bool search_lmp = true;             // SearchLMP：静かな手の数による枝刈り（改善で閾値を変える）
    bool search_see_prune = true;       // SearchSEEPrune：浅い深さで、SEE の悪い取る手・静かな手を刈る
    bool search_cont_hist = true;       // SearchContHistory：連続履歴（直前の手・2手前の手との組み合わせ）を並べ替えに使う
    bool search_mate1 = true;           // SearchMate1Ply：各節点で、1手で詰む手（王手）を調べる
    bool search_drop_prune = true;      // SearchDropPrune：浅い深さで、玉から遠く、どの駒にも利かない打つ手を刈る
    bool search_iir = false;             // SearchIIR：置換表に手が無いとき、1手浅く読む
    bool search_improving = false;       // SearchImproving：評価値が2手前より上がっているかで、枝刈りを調整する
    bool search_razoring = false;        // SearchRazoring：浅い深さで評価値が α を大きく下回るとき、静止探索で返す

    // 枝刈りの幅（USI の spin。持ち主が評価関数に合わせて調整する。点数は内部の単位。material_v0 は歩 約270点）
    int search_rfp_margin = 180;        // SearchRFPMargin：RFP の深さ1あたりの余裕
    int search_lmp_scale = 3;           // SearchLMPScale：LMP の手の数の係数（scale × (3 + 深さ²)）
    int search_razor_margin = 300;      // SearchRazorMargin：razoring の深さ1あたりの余裕
    int search_futility_margin = 250;   // SearchFutilityMargin：futility の深さ1あたりの余裕
    int search_see_margin = 40;         // SearchSEEMargin：SEE 枝刈りの静かな手の閾値（深さ1あたり）
    int search_rfp_depth = 3;           // SearchRFPDepth：RFP を使う最大の深さ（7 は長い詰将棋を 13 問に減らす。3 なら 16 問）

    // 並べ替え・静止探索の入り切り（既定は true。false なら N5H と同じ動き）
    bool search_order_see = true;       // SearchOrderSEE：駒を取る手を SEE で分ける（false なら MVV-LVA）
    bool search_history_gravity = false; // SearchHistoryGravity：履歴を減衰つきで更新し、試した静かな手を減点する（false なら加算のみ）
    bool search_counter_move = false;    // SearchCounterMove：カウンター手を並べ替えに使う
    bool search_qsee = false;            // SearchQSEE：静止探索で SEE が負の取る手を読まない（false なら N5 の簡易判定）
    bool search_lazy_legal = true;      // SearchLazyLegal：疑似合法手で生成し、指した後に自玉の安全を確かめる（false なら合法手で生成）
    bool search_rfp_guard = false;      // SearchRFPGuard：相手の玉の逃げ道が1つ以下の節点では RFP を使わない（N5.5 の測定で効果が無いため既定は false）

    // ヒントの使い方（DJP の hint_weight・hint_lmr_threshold。§E5.5）
    double hint_weight = 1.0;           // 0 なら、ヒントを並べ替えにも LMR にも使わない
    double hint_lmr_threshold = 0.10;   // この確率以上の手は、LMR で減らさない
};

// 評価に使う重み（どちらも nullptr なら評価できない）。Game が持つのと同じ重みを指す
struct EvalRef {
    const raypair::Network* net = nullptr;  // RayPair
    double scale = 0;                       // RayPair の eval_scale
    const halfkp::Network* hk = nullptr;    // HalfKP（§E10）
    int fv_scale = 16;                      // HalfKP の FV_SCALE（評価値 = raw / fv_scale）
};

// ルートの手ごとの記録（§E5.4）
struct RootMove {
    Move move{};
    int score = 0;          // 手番側から見た値（内部の単位）
    int bound = kExact;     // 正確な値か、下限・上限か
    int depth = 0;          // その値を得た深さ
    int seldepth = 0;       // 最大の深さ
    std::vector<Move> pv;   // 読み筋（この手から）
    long long nodes = 0;    // この手に使ったノード数（累計）
};

// ヒントの表（§E5.5）。局面（ハッシュ値）ごとに「手 → 確率」
struct HintEntry {
    Move move{};
    double prob = 0;
};
class HintTable {
public:
    void set(uint64_t key, std::vector<HintEntry> entries);
    const std::vector<HintEntry>* find(uint64_t key) const;
    void clear() { table_.clear(); order_.clear(); }
    size_t size() const { return table_.size(); }
    static constexpr size_t kMaxEntries = 65536;  // 既定の上限（§E5.5）。超えたら古いものから捨てる
private:
    std::unordered_map<uint64_t, std::vector<HintEntry>> table_;
    std::deque<uint64_t> order_;
};

// 置換表（§E5.3）。全ジョブで共有する。世代で古い項目から置き換える。
// 読み書きは1つの項目を2語で持ち、キーは「キーXOR データ」で保存する（ロックなしで壊れを検出する）。
struct TTEntry {
    bool found = false;
    int depth = 0;
    int score = 0;
    int bound = kExact;
    bool has_move = false;
    Move move{};
};

class TranspositionTable {
public:
    void resize(int mb);
    void clear();
    void new_generation() { gen_ = (uint8_t)(gen_ + 1); }
    bool probe(uint64_t key, TTEntry& e) const;
    void store(uint64_t key, int depth, int score, int bound, const Move& m, bool has_move);
    int hashfull() const;  // 千分率（先頭1000項目のうち今の世代のもの）
private:
    struct Slot {
        std::atomic<uint64_t> key_xor{0};
        std::atomic<uint64_t> data{0};
    };
    std::unique_ptr<Slot[]> slots_;
    size_t size_ = 0;
    uint8_t gen_ = 0;
};

// スレッドプール（§E5.2）。探索スレッドを起動時に作り、ジョブの作業（ヘルパー）を受け付ける。
// cpus があれば、ワーカーは cpus[(番号+1) % 個数] の論理CPUに固定する（番号 0 は駆動スレッドの CPU）。
class ThreadPool {
public:
    explicit ThreadPool(int n, std::vector<int> cpus = {});
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    int size() const { return (int)workers_.size(); }
    int pin_failures() const { return pin_failures_.load(); }  // 固定に失敗したワーカーの数
    void submit(std::function<void()> task);
private:
    void loop(int index);
    std::vector<int> cpus_;
    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool quit_ = false;
    int started_ = 0;                   // 固定を済ませたワーカーの数（mu_ で守る）
    std::atomic<int> pin_failures_{0};
};

// 出来事と情報の出力先（USI の info 行を1行ずつ渡す）
using InfoSink = std::function<void(const std::string&)>;

// 根の手ごとの値の報告（反復ごと。rm は並べ替え済み。Dajo の move の行に使う）
using RootReport = std::function<void(int depth, const std::vector<RootMove>& rm, long long nodes, long long ms)>;

// probe の結果（§E9.4）。verdict は 1＝yes（下限 ≥ T）、0＝no（上限 < T）、-1＝unknown
struct ProbeOut {
    int verdict = -1;
    int threshold = 0;
    int score = 0;          // 最後に読み切った段の値（手番側から見た内部の単位）
    int depth = 0;          // 最後に読み切った段
    std::vector<Move> pv;   // no のとき：この手を指した後の、相手の咎める手順（手は含まない）
};

// 1つのジョブ（§E5.2）。局面・制限・設定を持ち、Run で探索する。USI の go はジョブ番号0として扱う。
class Job {
public:
    Job(const Game& root, const Limits& lim, const Options& opt, const EvalRef& ev,
        TranspositionTable& tt, ThreadPool* pool, const HintTable* hints, bool null_root, InfoSink sink);
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    void run();                // 探索して結果を用意する（ブロッキング）
    void stop();               // 止める（止めた時点までの結果を返す）
    void ponder_hit();         // 予想が当たった：持ち時間の計測を始める（§E2.2 の 2）
    long long nodes() const;   // 使ったノード数（全スレッドの合計）
    const std::string& best_move() const { return best_move_; }      // "resign"・"win"・手
    const std::string& ponder_move() const { return ponder_move_; }  // 空なら無し
    const std::vector<RootMove>& result() const { return result_; }
    const ProbeOut& probe() const { return probe_; }                 // kProbe の結果
    bool mate_found() const { return mate_found_; }                   // kMate：詰みを見つけたか
    const std::vector<std::string>& mate_pv() const { return mate_pv_; }  // kMate：詰みの手順（USI）

    void set_root_report(RootReport f) { sh_.root_report = std::move(f); }
    // 動いているジョブの上限を後から付ける（DJP の promote）。time・nodes は今からの量、depth は最大の深さ
    void extend(long long time_ms, long long nodes, int depth);

    // 内部の共有状態（Worker から参照する）
    struct Shared {
        std::atomic<bool> stop{false};
        std::atomic<long long> deadline_ms{0};  // 0 は時間の制限なし
        std::atomic<long long> nodes{0};
        std::atomic<long long> node_limit{0};   // 0 は無し（promote で変わるので atomic）
        std::atomic<int> max_depth{kMaxDepth};  // 反復深化の上限（promote で変わる）
        TranspositionTable* tt = nullptr;
        const HintTable* hints = nullptr;
        Options opt;
        EvalRef ev;
        InfoSink sink;
        RootReport root_report;
        bool exact_all = false;                 // rank：この段で正確な値を得た手の値を、上限の値で上書きしない
        bool no_tt_read = false;                // probe：置換表の結論を読まない（他のジョブの深い結論に頼らず、自分の節点数で読む）
    };
private:
    long long budget_ms() const;   // 今の制限から思考時間（ms）を求める。制限なしは -1（§E2.4 の NetworkDelay を差し引く）
    void run_probe(Game& game, std::vector<RootMove>& rm);  // kProbe：窓 [T−1, T] で深さ順に読む
    void run_mate(const Game& game);                         // kMate：df-pn で詰みを探す

    std::atomic<bool> searching_{true};  // 根の詰み探索の続行（stop で false になる。詰み探索はこの合図を見る）
    Game root_;
    Limits lim_;
    ThreadPool* pool_;
    bool null_root_;
    Shared sh_;
    std::vector<RootMove> result_;
    std::string best_move_ = "resign";
    std::string ponder_move_;
    ProbeOut probe_;
    bool mate_found_ = false;
    std::vector<std::string> mate_pv_;
};

// 局面の評価（静的評価。qsearch なら静止探索の値）。内部の単位（手番側から見た cp。詰みは含みうる）。
// 置換表は一時的なものを使う（DJP の eval のジョブ）
int qsearch_score(const Game& g, const EvalRef& ev);

}  // namespace search
