// Dajo プロトコル DJP v0（設計書 §E9）。USI と同じ標準入出力で、dajo で始まる行だけを扱う。
// ジョブは番号で区別する。スレッドは Nuenagon のスレッドプールから threads 本ずつ割り当て、足りなければ優先度の順に待たせる。
#pragma once
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "search/search.h"

namespace dajo {

// 合議の窓口が使う、Nuenagon の状態（main が渡す）。設定を変えるのは main と Dajo だけ
struct Host {
    search::Options* opt = nullptr;                        // USI と同じ設定（dajo set で変える）
    search::TranspositionTable* tt = nullptr;
    std::shared_ptr<search::HintTable>* hints = nullptr;   // ヒントの表（ジョブは作った時点の複製を見る）
    std::function<search::ThreadPool*()> pool;             // 今のスレッドプール
    std::function<void()> reconfigure;                     // プールと置換表を作り直す（ジョブが無いときだけ呼ぶ）
    std::function<search::EvalRef()> eval;                 // 評価の重み（読み込んでいなければ空）
    std::function<std::string()> sysinfo;                  // dajo sysinfo の中身
    std::function<void(const std::string&)> out;           // 1行を出す（ロックしてから書く）
};

class Dajo {
public:
    // ジョブの終わりの内容（status 以外の値は extra に、USI 風の key=value で入れる）
    struct Outcome {
        std::string status = "done";
        std::string extra;
    };

    explicit Dajo(Host host);
    ~Dajo();
    Dajo(const Dajo&) = delete;
    Dajo& operator=(const Dajo&) = delete;

    // dajo で始まる1行を処理する。hello の前は hello だけを処理し、ほかは黙って捨てる（§E9.1）
    void handle(const std::string& line);
    bool busy();               // 動いている・待っているジョブがあるか
    void cancel_all();         // すべて止め、終わるまで待つ（USI の setoption・isready の前）
    void reap();               // 終わったジョブのスレッドを待ち、保留していた設定の変更を反映する

private:
    struct Entry;

    void out(const std::string& s);
    void error(long long id, const std::string& code, const std::string& extra = "");
    void hello(const std::vector<std::string>& t);
    void set(const std::vector<std::string>& t);
    void newgame();
    void cancel(const std::vector<std::string>& t);
    bool cancel_id(int id);
    void promote(const std::vector<std::string>& t);
    void hint(const std::vector<std::string>& t);
    void job(const std::vector<std::string>& t);
    bool in_use(int id);

    void admit(const std::shared_ptr<Entry>& e);
    void start_locked(const std::shared_ptr<Entry>& e);
    void pump_locked();
    void run_driver(const std::shared_ptr<Entry>& e);
    void finish(Entry& e, const Outcome& o);
    void join_done();
    bool run_part(Entry& e, const std::shared_ptr<search::Job>& j);
    std::shared_ptr<search::Job> make_job(Entry& e, const Game& g, const search::Limits& lim,
                                          const search::Options& opt, bool null_root);
    search::RootReport move_reporter(Entry& e, int lines);
    void print_moves(Entry& e, const std::vector<search::RootMove>& rm);

    Host host_;
    bool enabled_ = false;                                  // hello を受けたか（main の スレッドだけが触る）
    double eval_scale_ = 0;                                 // dajo set eval_scale（0 なら重みの既定）
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<int, std::shared_ptr<Entry>> active_;          // 待っているものと動いているもの（番号ごと）
    std::vector<std::shared_ptr<Entry>> queue_;             // 待っているジョブ（優先度の高い順、同じなら受けた順）
    int capacity_ = 1;                                      // スレッドの総数（Threads）
    int used_ = 0;                                          // 割り当て中のスレッド数
    int serial_ = 0;
    bool pending_reconfigure_ = false;                      // 動いているあいだに受けた、プールと置換表の変更
    std::map<int, std::thread> drivers_;                    // 動いているドライバ（serial で管理）
    std::vector<int> done_;                                 // 終わったドライバ（reap で待つ）
};

}  // namespace dajo
