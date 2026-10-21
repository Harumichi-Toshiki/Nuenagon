// 局面（盤面と履歴）。指し手は Board::push_move / undo_move で進める・戻す（差分更新）。
// 履歴は千日手の判定に使う。探索（N4）でも、この進める・戻す処理をそのまま使う。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
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
};

class Game {
public:
    Board board;

    Game();

    void set_startpos();
    void set_sfen(const std::string& sfen);  // 履歴は作り直す
    bool push_usi(const std::string& usi);   // 合法手と照合してから進める。違法なら false
    void push(const Move& m);                // 指し手を1手進め、履歴に積む
    void pop();                              // 直前の手を戻し、履歴から外す
    std::vector<std::string> legal_moves_usi() const;

    // 今の局面の千日手の状態（判定の決まりは cshogi の Board.is_draw() と同じ）
    Repetition repetition() const;

private:
    struct Ply {
        Move move;
        StateInfo state;
    };
    std::vector<Ply> plies_;
    std::vector<HistoryEntry> history_;  // history_.size() == plies_.size() + 1
};

// 指し手生成の数え上げ（perft）。board は差分更新で進めて戻す。
uint64_t perft(Board& board, int depth);
