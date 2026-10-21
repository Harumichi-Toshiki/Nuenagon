#include "game.h"

namespace {

HistoryEntry make_entry(const Board& b) {
    HistoryEntry e{};
    uint64_t hand_key = 0;
    for (int c = 0; c < 2; ++c) {
        for (int t = 0; t < 8; ++t) e.hand[c][t] = b.hand[c][t];
        for (int t = 1; t <= 7; ++t) hand_key ^= ZOBRIST_HAND[c][t][b.hand[c][t]];
    }
    e.key = b.zobrist_key;
    e.board_key = b.zobrist_key ^ hand_key;  // 全体のハッシュから持ち駒の分を外す
    e.turn = b.turn;
    e.in_check = b.in_check(b.turn);
    return e;
}

}  // namespace

const char* repetition_name(Repetition r) {
    switch (r) {
    case Repetition::Draw: return "draw";
    case Repetition::Win: return "win";
    case Repetition::Lose: return "lose";
    case Repetition::Superior: return "superior";
    case Repetition::Inferior: return "inferior";
    default: return "none";
    }
}

Game::Game() {
    set_startpos();
}

void Game::set_startpos() {
    set_sfen("startpos");
}

void Game::set_sfen(const std::string& sfen) {
    board.set_sfen(sfen);
    plies_.clear();
    history_.clear();
    history_.push_back(make_entry(board));
}

void Game::push(const Move& m) {
    plies_.push_back(Ply{ m, StateInfo{} });
    board.push_move(m, plies_.back().state);
    history_.push_back(make_entry(board));
}

void Game::pop() {
    const Ply& p = plies_.back();
    board.undo_move(p.move, p.state);
    plies_.pop_back();
    history_.pop_back();
}

bool Game::push_usi(const std::string& usi) {
    Move list[MAX_LEGAL_MOVES];
    Move* end = generate_moves(board.bit_pos, list);
    for (Move* m = list; m != end; ++m) {
        if (move_to_usi(*m) == usi) {
            push(*m);
            return true;
        }
    }
    return false;
}

std::vector<std::string> Game::legal_moves_usi() const {
    Move list[MAX_LEGAL_MOVES];
    Move* end = generate_moves(board.bit_pos, list);
    std::vector<std::string> out;
    for (Move* m = list; m != end; ++m) out.push_back(move_to_usi(*m));
    return out;
}

// 決まり（cshogi の is_draw() と同じ）：
//  1. 手番・盤面・両者の持ち駒が同じ局面が過去にあれば、その局面からの手の王手の有無で
//     「相手の手がすべて王手」なら Win、「手番側の手がすべて王手」なら Lose、それ以外は Draw。
//  2. 盤面と手番が同じで、手番側の持ち駒が過去の局面以上なら Superior、以下なら Inferior。
// 過去の局面は、手番が同じ（偶数手前）ものだけを、新しいものから順に調べる。
Repetition Game::repetition() const {
    const int n = (int)history_.size() - 1;
    const HistoryEntry& cur = history_[n];

    for (int j = n - 2; j >= 0; j -= 2) {
        if (history_[j].key != cur.key) continue;

        // j の局面から今までの間に、相手が指した手（j+2, ..., n の局面で手番側が王手されている）
        bool opp_all_check = true;
        for (int m = j + 2; m <= n; m += 2) {
            if (!history_[m].in_check) { opp_all_check = false; break; }
        }
        // j の局面から今までの間に、手番側が指した手（j+1, ..., n-1 の局面で相手が王手されている）
        bool own_all_check = true;
        for (int m = j + 1; m < n; m += 2) {
            if (!history_[m].in_check) { own_all_check = false; break; }
        }
        if (opp_all_check) return Repetition::Win;
        if (own_all_check) return Repetition::Lose;
        return Repetition::Draw;
    }

    for (int j = n - 2; j >= 0; j -= 2) {
        const HistoryEntry& past = history_[j];
        if (past.board_key != cur.board_key) continue;

        bool ge = true, le = true, gt = false, lt = false;
        for (int t = 1; t <= 7; ++t) {
            int now = cur.hand[cur.turn][t], was = past.hand[cur.turn][t];
            if (now < was) { ge = false; lt = true; }
            if (now > was) { le = false; gt = true; }
        }
        if (ge && gt) return Repetition::Superior;
        if (le && lt) return Repetition::Inferior;
    }
    return Repetition::None;
}

uint64_t perft(Board& board, int depth) {
    if (depth == 0) return 1;

    Move list[MAX_LEGAL_MOVES];
    Move* end = generate_moves(board.bit_pos, list);
    if (depth == 1) return (uint64_t)(end - list);

    uint64_t nodes = 0;
    for (Move* m = list; m != end; ++m) {
        StateInfo st;
        board.push_move(*m, st);
        nodes += perft(board, depth - 1);
        board.undo_move(*m, st);
    }
    return nodes;
}
