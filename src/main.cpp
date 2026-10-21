// Nuenagon の標準入出力の窓口。命令を1行ずつ読み、応答を1行ずつ返す（N1〜N3 の命令＋N4 の USI）。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "board/game.h"
#include "eval/raypair/network.h"
#include "eval/raypair/raypair.h"
#include "mate/dfpn.h"
#include "search/search.h"

namespace {

std::atomic<bool> g_searching{true};
std::mutex g_out_mu;  // 探索のスレッドからも出力するため

void reply(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_out_mu);
    std::cout << s << std::endl;  // 1行ごとに必ず流す
}

// "<head> <インデックス…>" の形で1行に返す
void reply_indices(const std::string& head, const std::vector<int>& idx) {
    std::string out = head;
    for (int t : idx) out += " " + std::to_string(t);
    reply(out);
}

// 評価関数の設定（USI オプション §E2.3）と、読み込んだ重み
struct EvalState {
    std::string file = "eval/nuenagon.shk";  // EvalFile
    bool file_set = false;                   // EvalFile を明示的に指定したか
    int scale_option = 0;                    // EvalScale（0 ならファイルの値）
    bool debug_check = false;                // DebugCheckAccumulator
    raypair::Network net;                    // 読み込んだ重み（loaded のとき）
    bool loaded = false;
    double scale = 0;                        // 評価に使う eval_scale
};

// features <v> <p> ／ linefeatures <v> <p> <L> ／ linesof <sq>（N2）。v：0 = v0、1 = v1。p：0 = 先手視点、1 = 後手視点
void cmd_features(const Game& game, std::istringstream& in) {
    int v = -1, p = -1;
    if (!(in >> v >> p) || v < 0 || v > 1 || p < 0 || p > 1) {
        reply("info string bad arguments");
        return;
    }
    std::vector<int> f;
    raypair::compute_features(game.board, p, v, f);
    reply_indices("features", f);
}

void cmd_linefeatures(const Game& game, std::istringstream& in) {
    int v = -1, p = -1, line = -1;
    if (!(in >> v >> p >> line) || v < 0 || v > 1 || p < 0 || p > 1 || line < 0 || line >= raypair::kLines) {
        reply("info string bad arguments");
        return;
    }
    std::vector<int> f;
    raypair::append_line_b1(raypair::make_view(game.board, p), v, line, f);
    std::sort(f.begin(), f.end());
    reply_indices("linefeatures", f);
}

void cmd_linesof(std::istringstream& in) {
    int sq = -1;
    if (!(in >> sq) || sq < 0 || sq > 80) {
        reply("info string bad arguments");
        return;
    }
    const int* l = raypair::kLinesOf.line[sq];
    reply_indices("linesof", std::vector<int>(l, l + 4));
}

// position startpos [moves …] ／ position sfen <盤面> <手番> <持ち駒> [<手数>] [moves …]
void cmd_position(Game& game, std::istringstream& in) {
    std::string kind, tok;
    in >> kind;
    if (kind == "startpos") {
        game.set_startpos();
    }
    else if (kind == "sfen") {
        std::string sfen;
        while (in >> tok) {
            if (tok == "moves") break;
            sfen += sfen.empty() ? tok : " " + tok;
        }
        game.set_sfen(sfen);
    }
    else {
        return;
    }
    while (in >> tok) {
        if (tok == "moves") continue;
        if (!game.push_usi(tok)) {
            reply("info string illegal move " + tok);
            return;
        }
    }
}

// setoption name <名前> value <値>（§E2.3）。応答はない
void cmd_setoption(EvalState& ev, search::Options& opt, std::istringstream& in) {
    std::string tok, name, value;
    in >> tok >> name;  // "name" <名前>
    in >> tok;          // "value"
    std::getline(in, value);
    value.erase(0, value.find_first_not_of(' '));
    value.erase(value.find_last_not_of(" \r") + 1);
    auto as_int = [&value](int def) {
        try { return std::stoi(value); } catch (...) { return def; }
    };
    if (name == "EvalFile") {
        ev.file = value;
        ev.file_set = true;
    }
    else if (name == "EvalScale") {
        ev.scale_option = std::max(0, as_int(0));
    }
    else if (name == "DebugCheckAccumulator") {
        ev.debug_check = (value == "true");
        opt.debug_check = ev.debug_check;
    }
    else if (name == "Threads") opt.threads = std::clamp(as_int(4), 1, 256);
    else if (name == "USI_Hash") opt.hash_mb = std::clamp(as_int(256), 1, 65536);
    else if (name == "MultiPV") opt.multipv = std::clamp(as_int(1), 1, 500);
    else if (name == "MaxPly") opt.max_ply = std::clamp(as_int(0), 0, 10000);
    else if (name == "DrawValue") opt.draw_value = std::clamp(as_int(0), -1000, 1000);
    else if (name == "NetworkDelay") opt.network_delay = std::clamp(as_int(120), 0, 10000);
    else if (name == "USI_Ponder") opt.usi_ponder = (value == "true");
    else if (name == "Deterministic") opt.deterministic = (value == "true");
    else if (name == "CpuAffinity") opt.cpu_affinity = value;     // 受け付けて覚えるだけ（N5 で使う）
    else if (name == "UsePhysicalCoresOnly") opt.physical_only = (value == "true");
}

// isready：重みを読み込む（§E2.5）。既定のファイルも無いときは評価なしで準備完了にする
void cmd_isready(Game& game, EvalState& ev) {
    reply(std::string("info string Nuenagon build: simd=") + (NUENAGON_SIMD_AVX2 ? "avx2" : "scalar"));
    if (!ev.file_set && !std::ifstream(ev.file).good()) {
        ev.loaded = false;
        game.set_network(nullptr);
        reply("info string eval: no weight file (" + ev.file + ")");
        reply("readyok");
        return;
    }
    raypair::Network net;
    std::string err;
    if (!raypair::load_network(ev.file, net, err)) {
        ev.loaded = false;
        game.set_network(nullptr);
        reply("info string eval load error: " + err);
        return;
    }
    ev.net = std::move(net);
    ev.loaded = true;
    ev.scale = ev.scale_option > 0 ? (double)(float)ev.scale_option : (double)ev.net.eval_scale;
    game.set_network(&ev.net);

    std::ostringstream info;
    info << std::fixed << std::setprecision(1);
    info << "info string Nuenagon eval: feature=" << ev.net.feature_set << " network=" << ev.net.network_id
         << " W=" << raypair::kW << " L1=" << ev.net.L1 << " L2=" << ev.net.L2
         << " eval_scale=" << ev.scale << (ev.scale_option > 0 ? " (option)" : " (file)");
    reply(info.str());
    reply("readyok");
}

// 差分で保っている蓄積値の検査（DebugCheckAccumulator が true のとき。§E3.4）
void debug_check(const Game& game, const EvalState& ev) {
    if (!ev.debug_check || !ev.loaded) return;
    if (!game.accumulator_matches_full()) {
        reply("info string accumulator mismatch " + game.board.to_sfen_key() + " " + game.last_move_usi());
    }
}

// eval → "eval raw <int> cp <int>"
void cmd_eval(const Game& game, const EvalState& ev) {
    if (!ev.loaded) {
        reply("info string eval not loaded");
        return;
    }
    int32_t raw = raypair::forward_raw(ev.net, game.accumulator(), game.board.turn);
    reply("eval raw " + std::to_string(raw) + " cp " + std::to_string(raypair::cp_of(raw, ev.scale)));
}

// acc <p> ／ accfull <p>：視点 p の蓄積値。acc は差分で保っている値、accfull は全計算
void cmd_acc(const Game& game, const EvalState& ev, std::istringstream& in, bool full) {
    int p = -1;
    if (!(in >> p) || p < 0 || p > 1) {
        reply("info string bad arguments");
        return;
    }
    if (!ev.loaded) {
        reply("info string eval not loaded");
        return;
    }
    raypair::Accumulator a;
    if (full) raypair::full_accumulator(ev.net, game.board, a);
    else a = game.accumulator();
    std::string out = full ? "accfull" : "acc";
    for (int j = 0; j < raypair::kW; j++) out += " " + std::to_string(a.v[p][j]);
    reply(out);
}

// acccheck <手数> <乱数の種>：ランダムに指し進め・戻し・null を混ぜ、毎回の差分更新を全計算と比べる（§E6-2）
void cmd_acccheck(Game& game, const EvalState& ev, std::istringstream& in) {
    long long steps = -1;
    unsigned long long seed = 0;
    if (!(in >> steps >> seed) || steps < 0) {
        reply("info string bad arguments");
        return;
    }
    if (!ev.loaded) {
        reply("info string eval not loaded");
        return;
    }
    std::mt19937_64 rng(seed);
    long long checks = 0;
    std::string last = "none";
    for (long long i = 0; i < steps; i++) {
        const int r = (int)(rng() % 100);
        std::vector<Move> moves = game.legal_moves();
        if (r < 10) {
            game.push_null();
            last = "null";
        }
        else if (r < 25 && game.ply_count() > 0) {
            game.pop();
            last = "undo";
        }
        else if (moves.empty()) {
            // 詰んでいるときは戻す。戻す手が無ければ開始局面に戻す
            if (game.ply_count() > 0) {
                game.pop();
                last = "undo";
            }
            else {
                game.set_startpos();
                last = "startpos";
            }
        }
        else {
            Move m = moves[rng() % moves.size()];
            game.push(m);
            last = game.last_move_usi();
        }
        if (!game.accumulator_matches_full()) {
            reply("acccheck fail " + game.board.to_sfen_key() + " " + last);
            return;
        }
        checks++;
    }
    reply("acccheck ok " + std::to_string(checks));
}

// evalbench <回数>：差分更新1手（両視点）の平均時間と、評価1回（隠れ層以降）の平均時間（マイクロ秒）
void cmd_evalbench(Game& game, const EvalState& ev, std::istringstream& in) {
    long long n = 0;
    if (!(in >> n) || n <= 0) {
        reply("info string bad arguments");
        return;
    }
    if (!ev.loaded) {
        reply("info string eval not loaded");
        return;
    }
    using clock = std::chrono::steady_clock;
    std::vector<Move> moves = game.legal_moves();
    if (moves.empty()) {
        reply("info string no legal moves");
        return;
    }
    double push_us = 0, pop_us = 0;
    for (long long i = 0; i < n; i++) {
        Move m = moves[i % moves.size()];
        auto t0 = clock::now();
        game.push(m);
        auto t1 = clock::now();
        game.pop();
        auto t2 = clock::now();
        push_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        pop_us += std::chrono::duration<double, std::micro>(t2 - t1).count();
    }
    long long sum = 0;  // 最適化で評価を消されないように、結果を足して出す
    auto t0 = clock::now();
    for (long long i = 0; i < n; i++) {
        sum += raypair::forward_raw(ev.net, game.accumulator(), game.board.turn);
    }
    auto t1 = clock::now();
    double eval_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    std::ostringstream out;
    out << std::fixed << std::setprecision(3)
        << "info string evalbench push_us=" << push_us / n << " pop_us=" << pop_us / n
        << " eval_us=" << eval_us / n << " n=" << n << " sum=" << sum;
    reply(out.str());
}

// ---------- 探索（§E2、§E5）----------

// 探索の状態。ジョブは専用のスレッド（driver）で動き、bestmove はそのスレッドから出す。
// 同時に動かすのは1つ（main の go）。ジョブの仕組み自体は複数のジョブに対応している（§E5.2）。
struct Engine {
    search::Options opt;
    search::TranspositionTable tt;
    search::HintTable hints;
    std::unique_ptr<search::ThreadPool> pool;
    int pool_size = -1;
    int tt_mb = -1;
    std::shared_ptr<search::Job> job;
    std::thread driver;

    // 動いているジョブを止めて、終わるまで待つ
    void stop_job() {
        if (job) job->stop();
        wait_job();
    }
    // 動いているジョブが終わるまで待つ（止めない）
    void wait_job() {
        if (driver.joinable()) driver.join();
        job.reset();
    }
    ~Engine() { stop_job(); }
};

// 置換表とスレッドプールを、設定（Threads・USI_Hash）に合わせる。ジョブが無いときに呼ぶ
void apply_config(Engine& e) {
    if (e.tt_mb != e.opt.hash_mb) {
        e.tt.resize(e.opt.hash_mb);
        e.tt_mb = e.opt.hash_mb;
    }
    const int want = std::max(0, e.opt.threads - 1);  // 探索スレッドは main（driver）＋ helper
    if (e.pool_size != want) {
        e.pool.reset(new search::ThreadPool(want));
        e.pool_size = want;
    }
}

// go <引数…>（§E2.1）。ジョブ番号0として起動する
void cmd_go(Engine& e, const EvalState& ev, const Game& game, std::istringstream& in) {
    e.stop_job();
    search::Limits lim;
    auto num = [&in]() {
        long long v = 0;
        in >> v;
        return v;
    };
    std::string tok;
    bool in_moves = false;
    while (in >> tok) {
        if (tok == "searchmoves") in_moves = true;
        else if (tok == "ponder") { lim.ponder = true; in_moves = false; }
        else if (tok == "infinite") { lim.infinite = true; in_moves = false; }
        else if (tok == "btime") { lim.btime = num(); in_moves = false; }
        else if (tok == "wtime") { lim.wtime = num(); in_moves = false; }
        else if (tok == "binc") { lim.binc = num(); in_moves = false; }
        else if (tok == "winc") { lim.winc = num(); in_moves = false; }
        else if (tok == "byoyomi") { lim.byoyomi = num(); in_moves = false; }
        else if (tok == "movetime") { lim.movetime = num(); in_moves = false; }
        else if (tok == "nodes") { lim.nodes = num(); in_moves = false; }
        else if (tok == "depth") { lim.depth = (int)num(); in_moves = false; }
        else if (tok == "mate") { num(); in_moves = false; }
        else if (in_moves) lim.searchmoves.push_back(tok);
    }

    // 重みが無いときも落ちず、bestmove を返す（§E2、テスト）
    if (!ev.loaded) {
        reply("info string eval not loaded");
        const std::vector<Move> moves = game.legal_moves();
        reply(moves.empty() ? "bestmove resign" : "bestmove " + move_to_usi(moves[0]));
        return;
    }

    apply_config(e);
    e.tt.new_generation();  // 世代を進める（§E5.3）
    const search::EvalRef ref{&ev.net, ev.scale};
    auto job = std::make_shared<search::Job>(game, lim, e.opt, ref, e.tt, e.pool.get(), &e.hints, false,
                                             [](const std::string& s) { reply(s); });
    e.job = job;
    e.driver = std::thread([job] {
        job->run();
        std::string line = "bestmove " + job->best_move();
        if (!job->ponder_move().empty()) line += " ponder " + job->ponder_move();
        reply(line);
    });
}

// bench [スレッド数]：固定の局面群を深さ6で探索した合計のノード数と NPS（§E6-10）
void cmd_bench(Engine& e, const EvalState& ev, std::istringstream& in) {
    e.stop_job();
    int threads = 1;
    in >> threads;
    if (!ev.loaded) {
        reply("bench nodes 0 nps 0");
        return;
    }
    static const char* kPositions[] = {
        "",
        "7g7f 3c3d 2g2f 8c8d",
        "7g7f 8c8d 2g2f 3c3d 2f2e 2b2c",
        "2g2f 8c8d 7g7f 3c3d 6i7h 4a3b",
    };
    search::Options o = e.opt;
    o.threads = std::clamp(threads, 1, 256);
    o.deterministic = false;
    std::unique_ptr<search::ThreadPool> bench_pool(new search::ThreadPool(o.threads - 1));
    e.tt.clear();
    const search::EvalRef ref{&ev.net, ev.scale};

    long long nodes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const char* mv : kPositions) {
        Game g;
        g.set_network(&ev.net);
        g.set_startpos();
        std::istringstream ms(mv);
        std::string u;
        while (ms >> u) g.push_usi(u);
        search::Limits lim;
        lim.depth = 6;
        search::Job job(g, lim, o, ref, e.tt, bench_pool.get(), nullptr, false, nullptr);
        job.run();
        nodes += job.nodes();
    }
    const long long ms = std::max<long long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now() - t0).count());
    reply("bench nodes " + std::to_string(nodes) + " nps " + std::to_string(nodes * 1000 / ms));
}

}  // namespace

int main() {
    init_bitboards();
    init_zobrist();

    Game game;
    EvalState ev;
    Engine eng;
    DfpnSearcher mate_search(&g_searching);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;

        if (cmd == "usi") {
            reply("id name Nuenagon");
            reply("id author Harumichi-Toshiki");
            reply("option name Threads type spin default 4 min 1 max 256");
            reply("option name CpuAffinity type string default auto");
            reply("option name UsePhysicalCoresOnly type check default false");
            reply("option name USI_Hash type spin default 256 min 1 max 65536");
            reply("option name EvalFile type string default eval/nuenagon.shk");
            reply("option name EvalScale type spin default 0 min 0 max 10000");
            reply("option name NetworkDelay type spin default 120 min 0 max 10000");
            reply("option name USI_Ponder type check default false");
            reply("option name MaxPly type spin default 0 min 0 max 10000");
            reply("option name DrawValue type spin default 0 min -1000 max 1000");
            reply("option name MultiPV type spin default 1 min 1 max 500");
            reply("option name Deterministic type check default false");
            reply("option name DebugCheckAccumulator type check default false");
            reply("usiok");
        }
        else if (cmd == "setoption") {
            eng.stop_job();
            cmd_setoption(ev, eng.opt, in);
        }
        else if (cmd == "isready") {
            eng.stop_job();
            cmd_isready(game, ev);
            if (ev.loaded) {
                apply_config(eng);
                reply("info string Nuenagon cpu: logical=" + std::to_string(std::thread::hardware_concurrency()) +
                      " threads=" + std::to_string(eng.opt.threads) + " affinity=" + eng.opt.cpu_affinity);
            }
        }
        else if (cmd == "usinewgame") {
            eng.stop_job();
            eng.tt.clear();   // 新しい対局：置換表と、ヒントの表を消す（§E5.3、§E5.5）
            eng.hints.clear();
        }
        else if (cmd == "go") {
            cmd_go(eng, ev, game, in);
        }
        else if (cmd == "stop") {
            if (eng.job) eng.job->stop();  // bestmove は探索のスレッドが出す
        }
        else if (cmd == "ponderhit") {
            if (eng.job) eng.job->ponder_hit();
        }
        else if (cmd == "gameover") {
            eng.stop_job();
        }
        else if (cmd == "bench") {
            cmd_bench(eng, ev, in);
        }
        else if (cmd == "position") {
            eng.stop_job();
            cmd_position(game, in);
            debug_check(game, ev);
        }
        else if (cmd == "undo") {
            game.pop();
            debug_check(game, ev);
        }
        else if (cmd == "null") {
            game.push_null();
            debug_check(game, ev);
        }
        else if (cmd == "eval") {
            cmd_eval(game, ev);
        }
        else if (cmd == "acc") {
            cmd_acc(game, ev, in, false);
        }
        else if (cmd == "accfull") {
            cmd_acc(game, ev, in, true);
        }
        else if (cmd == "acccheck") {
            cmd_acccheck(game, ev, in);
        }
        else if (cmd == "evalbench") {
            cmd_evalbench(game, ev, in);
        }
        else if (cmd == "sfen") {
            reply("sfen " + game.board.to_sfen_key());
        }
        else if (cmd == "moves") {
            std::string out = "moves";
            for (const std::string& m : game.legal_moves_usi()) out += " " + m;
            reply(out);
        }
        else if (cmd == "features") {
            cmd_features(game, in);
        }
        else if (cmd == "linefeatures") {
            cmd_linefeatures(game, in);
        }
        else if (cmd == "linesof") {
            cmd_linesof(in);
        }
        else if (cmd == "perft") {
            int depth = 0;
            if (in >> depth) reply("perft " + std::to_string(perft(game.board, depth)));
        }
        else if (cmd == "key") {
            std::ostringstream hex;
            hex << "key " << std::hex << std::setw(16) << std::setfill('0') << game.board.zobrist_key;
            reply(hex.str());
        }
        else if (cmd == "rep") {
            reply(std::string("rep ") + repetition_name(game.repetition()));
        }
        else if (cmd == "mate") {
            long long max_nodes = 0;
            if (!(in >> max_nodes)) max_nodes = 1000000;
            mate_search.clear();
            mate_search.set_max_nodes(max_nodes);
            g_searching = true;
            bool found = mate_search.search_mate(game.board);
            reply(found ? "mate " + mate_search.get_best_mate() : "mate none");
        }
        else if (cmd == "quit") {
            eng.stop_job();
            break;
        }
        else if (!cmd.empty()) {
            reply("info string unknown command " + cmd);  // 知らない命令は無視する
        }
    }
    eng.wait_job();  // 入力が終わったときは、動いている探索を終わるまで待つ
    return 0;
}
