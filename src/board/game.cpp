#include "game.h"
#include <cstring>

namespace {

HistoryEntry make_entry(const Board& b, bool is_null) {
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
    e.is_null = is_null;
    return e;
}

raypair::Accumulator full_of(const raypair::Network& net, const Board& b) {
    raypair::Accumulator acc;
    raypair::full_accumulator(net, b, acc);
    return acc;
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

void Game::set_network(const raypair::Network* net) {
    // 蓄積値は重みに依存するので、開始局面から指し手を進め直して作り直す
    std::vector<Ply> old = plies_;
    net_ = net;
    hk_ = nullptr;
    replay_from_start(old);
}

void Game::set_halfkp(const halfkp::Network* net) {
    std::vector<Ply> old = plies_;
    hk_ = net;
    net_ = nullptr;
    replay_from_start(old);
}

void Game::replay_from_start(const std::vector<Ply>& plies) {
    board = start_;
    this->plies_.clear();
    history_.clear();
    history_.push_back(make_entry(board, false));
    init_accumulators();
    for (const Ply& p : plies) {
        if (p.is_null) push_null();
        else push(p.move);
    }
}

void Game::init_accumulators() {
    acc_.clear();
    hacc_.clear();
    if (net_) acc_.push_back(full_of(*net_, board));
    if (hk_) {
        halfkp::Accumulator a;
        halfkp::full_accumulator(*hk_, board, a);
        hacc_.push_back(a);
    }
    refresh_views();
}

int32_t Game::raw_eval() const {
    if (hk_) return halfkp::forward_raw(*hk_, hacc_.back(), board.turn);
    if (!net_) return 0;
    return raypair::forward_raw(*net_, acc_.back(), board.turn);
}

void Game::set_startpos() {
    set_sfen("startpos");
}

void Game::set_sfen(const std::string& sfen) {
    board.set_sfen(sfen);
    start_ = board;
    plies_.clear();
    history_.clear();
    history_.push_back(make_entry(board, false));
    init_accumulators();
}

void Game::refresh_views() {
    if (!net_) return;
    for (int p = 0; p < 2; p++) view_[p] = raypair::make_view(board, p);
}

void Game::push(const Move& m) {
    // HalfKP の差分更新は、指す前の盤から取る情報が要る
    const halfkp::MoveInfo mi = hk_ ? halfkp::make_move_info(board, m) : halfkp::MoveInfo{};
    plies_.push_back(Ply{ m, StateInfo{}, false });
    board.push_move(m, plies_.back().state);
    history_.push_back(make_entry(board, false));
    if (hk_) {
        halfkp::Accumulator next = hacc_.back();
        halfkp::update_accumulator(*hk_, board, mi, hacc_.back(), next);
        hacc_.push_back(next);
    }
    if (net_) {
        // 変わったマスは移動元（駒打ちなら無し）と移動先。両視点の盤面を写し、その差分で蓄積値を更新する
        const int sqs[2] = { m.is_drop ? -1 : m.from, m.to };
        raypair::Accumulator next = acc_.back();
        for (int p = 0; p < 2; p++) {
            raypair::ViewDelta d;
            raypair::set_view_squares(view_[p], p, board, sqs, 2, &d);
            raypair::update_accumulator(*net_, p, view_[p], d, next);
        }
        acc_.push_back(next);
    }
}

void Game::push_null() {
    plies_.push_back(Ply{ Move{}, StateInfo{}, true });
    StateInfo& st = plies_.back().state;
    st.previous_zobrist_key = board.zobrist_key;
    st.previous_bit_pos = board.bit_pos;
    // 盤面は変わらない。手番だけ入れ替える（蓄積値は視点ごとなので変わらない。§E3.2）
    board.turn = 1 - board.turn;
    board.move_count++;
    board.zobrist_key ^= ZOBRIST_TURN;
    board.update_bitboard();
    history_.push_back(make_entry(board, true));
    if (net_) {
        raypair::Accumulator next = acc_.back();
        acc_.push_back(next);
    }
    if (hk_) hacc_.push_back(hacc_.back());
}

void Game::pop() {
    if (plies_.empty()) return;
    const Ply& p = plies_.back();
    if (p.is_null) {
        board.turn = 1 - board.turn;
        board.move_count--;
        board.zobrist_key = p.state.previous_zobrist_key;
        board.bit_pos = p.state.previous_bit_pos;
    }
    else {
        board.undo_move(p.move, p.state);
    }
    if (net_ && !p.is_null) {
        // 戻した盤の、変わったマスを両視点の盤面へ写し直す（蓄積値は acc_ のスタックから戻す）
        const int sqs[2] = { p.move.is_drop ? -1 : p.move.from, p.move.to };
        for (int v = 0; v < 2; v++) raypair::set_view_squares(view_[v], v, board, sqs, 2, nullptr);
    }
    plies_.pop_back();
    history_.pop_back();
    if (net_) acc_.pop_back();
    if (hk_) hacc_.pop_back();
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

std::string Game::last_move_usi() const {
    if (plies_.empty()) return "none";
    return plies_.back().is_null ? "null" : move_to_usi(plies_.back().move);
}

std::vector<std::string> Game::legal_moves_usi() const {
    Move list[MAX_LEGAL_MOVES];
    Move* end = generate_moves(board.bit_pos, list);
    std::vector<std::string> out;
    for (Move* m = list; m != end; ++m) out.push_back(move_to_usi(*m));
    return out;
}

std::vector<Move> Game::legal_moves() const {
    Move list[MAX_LEGAL_MOVES];
    Move* end = generate_moves(board.bit_pos, list);
    return std::vector<Move>(list, end);
}

bool Game::accumulator_matches_full() const {
    if (hk_) {
        halfkp::Accumulator full;
        halfkp::full_accumulator(*hk_, board, full);
        return std::memcmp(&full, &hacc_.back(), sizeof full) == 0;
    }
    if (!net_) return false;
    raypair::Accumulator full = full_of(*net_, board);
    return std::memcmp(&full, &acc_.back(), sizeof full) == 0;
}

// 決まり（cshogi の is_draw() と同じ）：
//  1. 手番・盤面・両者の持ち駒が同じ局面が過去にあれば、その局面からの手の王手の有無で
//     「相手の手がすべて王手」なら Win、「手番側の手がすべて王手」なら Lose、それ以外は Draw。
//  2. 盤面と手番が同じで、手番側の持ち駒が過去の局面以上なら Superior、以下なら Inferior。
// 過去の局面は、手番が同じ（偶数手前）ものだけを、新しいものから順に調べる。
// null move の局面より前は調べない（null は手に数えず、そこで履歴を区切る。§E5.6）。
Repetition Game::repetition() const {
    const int n = (int)history_.size() - 1;
    const HistoryEntry& cur = history_[n];

    int lo = 0;
    for (int i = n; i > 0; i--) {
        if (history_[i].is_null) { lo = i; break; }
    }

    for (int j = n - 2; j >= lo; j -= 2) {
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

    for (int j = n - 2; j >= lo; j -= 2) {
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
