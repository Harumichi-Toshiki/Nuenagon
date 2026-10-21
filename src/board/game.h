// 局面（盤面と履歴）。指し手は Game::push / pop で進める・戻す（差分更新）。パス（null move）は push_null / pop。
// 履歴は千日手の判定に使う。探索（N4）でも、この進める・戻す処理をそのまま使う。
// 重み（Network）を設定しているときは、第1層の蓄積値も同じ処理で差分更新し、戻すときはスタックから戻す。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "eval/raypair/network.h"
#include "shogi_utils.h"

enum class Repetition { None, Draw, Win, Lose, Superior, Inferior };

const char* repetition_name(Repetition r);

// 千日手の判定に使う、局面ごとの記録
struct HistoryEntry {
    uint64_t key;        // 全体のハッシュ値（盤面・持ち駒・手番）
    uint64_t board_key;  // 持ち駒を除いたハッシュ値（盤面・手番）
    int turn;            // 手番
    bool in_check;       // 手番側が王手されているか
    int hand[2][8];      // 持ち駒 [色][駒種]（駒種は 1..7）
    bool is_null;        // null move を指した後の局面
};

class Game {
public:
    Board board;

    Game();

    void set_network(const raypair::Network* net);  // 評価に使う重み（nullptr なら評価なし）。蓄積値を作り直す
    bool has_network() const { return net_ != nullptr; }

    void set_startpos();
    void set_sfen(const std::string& sfen);  // 履歴は作り直す
    bool push_usi(const std::string& usi);   // 合法手と照合してから進める。違法なら false
    void push(const Move& m);                // 指し手を1手進め、履歴に積む
    void push_null();                        // 手番だけを入れ替える（null move。千日手の判定では手に数えない）
    void pop();                              // 直前の手（null を含む）を戻し、履歴から外す。手が無ければ何もしない
    int ply_count() const { return (int)plies_.size(); }
    std::string last_move_usi() const;       // 直前の手（"null"、手が無ければ "none"）
    std::vector<std::string> legal_moves_usi() const;
    std::vector<Move> legal_moves() const;
    // 合法手を out に書き、その終わりを返す（out は MAX_LEGAL_MOVES 以上）。確保はしない
    Move* gen_legal(Move* out) const { return generate_moves(board.bit_pos, out); }

    // 現在の第1層の蓄積値（重みを設定しているときだけ使える）
    const raypair::Accumulator& accumulator() const { return acc_.back(); }
    // 差分で保っている蓄積値が、全計算と一致するか（§E3.4）
    bool accumulator_matches_full() const;

    // 今の局面で手番側が王手されているか（進めるときに求めた値を使う。board.in_check と同じ）
    bool in_check_now() const { return history_.back().in_check; }

    // 今の局面の千日手の状態（判定の決まりは cshogi の Board.is_draw() と同じ）
    Repetition repetition() const;

private:
    struct Ply {
        Move move;
        StateInfo state;
        bool is_null;
    };
    void refresh_views();                // 両視点の盤面を、今の盤から作り直す（重みがあるときだけ）
    std::vector<Ply> plies_;
    std::vector<HistoryEntry> history_;  // history_.size() == plies_.size() + 1
    Board start_;                        // 履歴の先頭の局面（重みを設定し直すときに使う）
    const raypair::Network* net_ = nullptr;
    std::vector<raypair::Accumulator> acc_;  // 重みがあるとき acc_.size() == plies_.size() + 1。戻す用のスタック
    // 両視点の盤面（差分更新の元）。指す・戻すたびに、変わったマスと持ち駒だけを写す（§E3.2）
    raypair::ViewBoard view_[2];
};

// 指し手生成の数え上げ（perft）。board は差分更新で進めて戻す。
uint64_t perft(Board& board, int depth);
