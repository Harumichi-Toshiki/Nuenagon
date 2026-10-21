// Nuenagon の標準入出力の窓口（N1・N2 の範囲）。命令を1行ずつ読み、応答を1行ずつ返す。
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "board/game.h"
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

}  // namespace

int main() {
    init_bitboards();
    init_zobrist();

    Game game;
    DfpnSearcher mate_search(&g_searching);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;

        if (cmd == "usi") {
            reply("id name Nuenagon");
            reply("id author Harumichi-Toshiki");
            reply("usiok");
        }
        else if (cmd == "isready") {
            reply("readyok");
        }
        else if (cmd == "position") {
            cmd_position(game, in);
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
