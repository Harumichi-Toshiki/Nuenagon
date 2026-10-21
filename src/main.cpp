// Nuenagon の標準入出力の窓口（N1〜N3 の範囲）。命令を1行ずつ読み、応答を1行ずつ返す。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "board/game.h"
#include "eval/raypair/network.h"
#include "eval/raypair/raypair.h"
#include "mate/dfpn.h"

namespace {

std::atomic<bool> g_searching{true};

void reply(const std::string& s) {
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
void cmd_setoption(EvalState& ev, std::istringstream& in) {
    std::string tok, name, value;
    in >> tok >> name;  // "name" <名前>
    in >> tok;          // "value"
    std::getline(in, value);
    value.erase(0, value.find_first_not_of(' '));
    value.erase(value.find_last_not_of(" \r") + 1);
    if (name == "EvalFile") {
        ev.file = value;
        ev.file_set = true;
    }
    else if (name == "EvalScale") {
        try { ev.scale_option = std::stoi(value); } catch (...) { ev.scale_option = 0; }
    }
    else if (name == "DebugCheckAccumulator") {
        ev.debug_check = (value == "true");
    }
}

// isready：重みを読み込む（§E2.5）。既定のファイルも無いときは評価なしで準備完了にする
void cmd_isready(Game& game, EvalState& ev) {
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

}  // namespace

int main() {
    init_bitboards();
    init_zobrist();

    Game game;
    EvalState ev;
    DfpnSearcher mate_search(&g_searching);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;

        if (cmd == "usi") {
            reply("id name Nuenagon");
            reply("id author Harumichi-Toshiki");
            reply("option name EvalFile type string default eval/nuenagon.shk");
            reply("option name EvalScale type spin default 0 min 0 max 10000");
            reply("option name DebugCheckAccumulator type check default false");
            reply("usiok");
        }
        else if (cmd == "setoption") {
            cmd_setoption(ev, in);
        }
        else if (cmd == "isready") {
            cmd_isready(game, ev);
        }
        else if (cmd == "position") {
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
            break;
        }
        else if (!cmd.empty()) {
            reply("info string unknown command " + cmd);  // 知らない命令は無視する
        }
    }
    return 0;
}
