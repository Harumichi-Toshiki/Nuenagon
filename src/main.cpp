// Nuenagon の標準入出力の窓口（N1 の範囲）。命令を1行ずつ読み、応答を1行ずつ返す。
#include <atomic>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include "board/game.h"
#include "mate/dfpn.h"

namespace {

std::atomic<bool> g_searching{true};

void reply(const std::string& s) {
    std::cout << s << std::endl;  // 1行ごとに必ず流す
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
