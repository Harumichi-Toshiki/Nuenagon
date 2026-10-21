#pragma once
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>
#include "board/shogi_utils.h"
#include "board/zobrist.h"

const int DFPN_INF = 999999;
inline int DFPN_TT_SIZE = 1 << 20; // 2 のべき乗（インデックスをマスクで求めるため）

struct DfpnNode {
    uint64_t key = 0;
    int pn = 1;
    int dn = 1;
    int mate_length = 0; // 最短・最長手数の管理用
    bool is_expanded = false;
    std::vector<Move> moves;
    std::vector<uint64_t> child_keys;
};

class DfpnSearcher {
private:
    std::vector<DfpnNode> tt;
    std::atomic<bool>* searching_flag;
    bool abort_search = false;
    std::string best_mate_move = "";
    int mate_length = 0;
    long long node_count = 0;

public:
    long long max_nodes = 10000000;
    void set_searching_flag(std::atomic<bool>* flag) { searching_flag = flag; }
    void set_max_nodes(long long limit) { max_nodes = limit; }
    void resize_hash() {
        tt.clear();
        tt.resize(DFPN_TT_SIZE);
    }
    DfpnSearcher(std::atomic<bool>* flag) : searching_flag(flag) {
        tt.resize(DFPN_TT_SIZE);
    }

    std::string get_best_mate() const { return best_mate_move; }
    // 見つけた詰みの読み筋の長さ（手番側の初手から、手の数）。見つからなければ 0（N4 の根の詰み探索で使う）
    int get_mate_length() const { return mate_length; }

    bool search_mate(const Board& root_board) {
        best_mate_move = "";
        mate_length = 0;
        node_count = 0;
        abort_search = false;

        uint64_t root_key = root_board.get_zobrist_key();
        std::vector<uint64_t> path_keys;
        path_keys.reserve(250);

        // ★修正：Boardをコピーして渡し、再帰内では参照を使う
        Board temp_root = root_board;
        mid(temp_root, true, DFPN_INF, DFPN_INF, 0, path_keys);

        int root_idx = root_key & (DFPN_TT_SIZE - 1);
        if (tt[root_idx].key == root_key && tt[root_idx].pn == 0) {
            // 1. まず通常のPV復元を試みる
            std::vector<std::string> pv = get_mate_pv(root_board);
            if (!pv.empty()) {
                best_mate_move = pv[0];
                mate_length = (int)pv.size();
            }
            else {
                // ★追加：ハッシュ衝突でPVが途切れた場合の強力なフォールバック
                // rootノードの展開済み手の中から、pn == 0 (詰み) となっている子ノードを直接探し出す
                for (size_t i = 0; i < tt[root_idx].moves.size(); ++i) {
                    uint64_t ck = tt[root_idx].child_keys[i];
                    int c_idx = ck & (DFPN_TT_SIZE - 1);
                    if (tt[c_idx].key == ck && tt[c_idx].pn == 0) {
                        best_mate_move = move_to_usi(tt[root_idx].moves[i]);
                        break;
                    }
                }
            }
            // 詰み手が見つからなければ false（「最初の手」は返さない）
            return !best_mate_move.empty();
        }
        return false;
    }

    void clear() {
        for (size_t i = 0; i < DFPN_TT_SIZE; ++i) {
            tt[i].key = 0;
            tt[i].is_expanded = false;
            // メモリを完全に解放する Swap Trick
            std::vector<Move>().swap(tt[i].moves);
            std::vector<uint64_t>().swap(tt[i].child_keys);
        }
    }

    std::vector<std::string> get_mate_pv(Board root_board) {
        std::vector<std::string> pv;
        Board temp_board = root_board;
        bool is_or_node = true;

        for (int depth = 0; depth < 100; ++depth) {
            uint64_t key = temp_board.get_zobrist_key();
            int idx = key & (DFPN_TT_SIZE - 1);
            if (tt[idx].key != key || tt[idx].pn != 0) break;

            int best_c = -1;
            if (is_or_node) {
                int min_len = 999999;
                for (size_t i = 0; i < tt[idx].moves.size(); ++i) {
                    uint64_t ck = tt[idx].child_keys[i];
                    int c_idx = ck & (DFPN_TT_SIZE - 1);
                    if (tt[c_idx].key == ck && tt[c_idx].pn == 0) {
                        if (tt[c_idx].mate_length < min_len) { min_len = tt[c_idx].mate_length; best_c = (int)i; }
                    }
                }
            }
            else {
                int max_len = -1;
                for (size_t i = 0; i < tt[idx].moves.size(); ++i) {
                    uint64_t ck = tt[idx].child_keys[i];
                    int c_idx = ck & (DFPN_TT_SIZE - 1);
                    if (tt[c_idx].key == ck && tt[c_idx].pn == 0) {
                        if (tt[c_idx].mate_length > max_len) { max_len = tt[c_idx].mate_length; best_c = (int)i; }
                    }
                }
            }
            if (best_c == -1) break;
            Move m = tt[idx].moves[best_c];
            pv.push_back(move_to_usi(m));
            StateInfo st;
            temp_board.push_move(m, st);
            is_or_node = !is_or_node;
        }
        return pv;
    }
    

private:
    // ★修正：Boardを「参照(&)」で受けることでスタック消費を劇的に抑える
    void mid(Board& board, bool is_or_node, int pn_th, int dn_th, int depth, std::vector<uint64_t>& path_keys) {
        if (!searching_flag->load() || abort_search) return;
        if (++node_count > max_nodes || depth > 200) { abort_search = true; return; }

        uint64_t key = board.get_zobrist_key();
        int idx = key & (DFPN_TT_SIZE - 1);

        // ノードの展開
        if (tt[idx].key != key || !tt[idx].is_expanded) {
            tt[idx].key = key;
            tt[idx].moves = board.get_tsume_moves_fast(is_or_node);
            tt[idx].child_keys.clear();
            for (const auto& m : tt[idx].moves) {
                StateInfo st;
                board.push_move(m, st);  // 手を進める
                tt[idx].child_keys.push_back(board.get_zobrist_key());
                board.undo_move(m, st);  // すぐに戻す
            }
            tt[idx].is_expanded = true;
            tt[idx].mate_length = 0;

            if (tt[idx].moves.empty()) {
                tt[idx].pn = is_or_node ? DFPN_INF : 0;
                tt[idx].dn = is_or_node ? 0 : DFPN_INF;
                return;
            }
            tt[idx].pn = is_or_node ? 1 : (int)tt[idx].moves.size();
            tt[idx].dn = is_or_node ? (int)tt[idx].moves.size() : 1;
        }

        if (tt[idx].pn == 0 || tt[idx].dn == 0) return;

        path_keys.push_back(key);

        while (tt[idx].pn < pn_th && tt[idx].dn < dn_th && searching_flag->load() && !abort_search) {
            int best_c = -1;
            int min_pn = DFPN_INF, min_pn2 = DFPN_INF;
            int min_dn = DFPN_INF, min_dn2 = DFPN_INF;
            long long sum_pn = 0, sum_dn = 0;

            for (size_t i = 0; i < tt[idx].moves.size(); ++i) {
                uint64_t child_key = tt[idx].child_keys[i];
                // ★修正：ループ検出時に即座にPN/DNを返し、無限ループを回避
                bool is_loop = (std::find(path_keys.begin(), path_keys.end(), child_key) != path_keys.end());
                int c_idx = child_key & (DFPN_TT_SIZE - 1);
                int c_pn = is_loop ? DFPN_INF : ((tt[c_idx].key == child_key) ? tt[c_idx].pn : 1);
                int c_dn = is_loop ? 0 : ((tt[c_idx].key == child_key) ? tt[c_idx].dn : 1);

                sum_pn = (std::min)((long long)DFPN_INF, sum_pn + c_pn);
                sum_dn = (std::min)((long long)DFPN_INF, sum_dn + c_dn);

                if (is_or_node) {
                    if (c_pn < min_pn) { min_pn2 = min_pn; min_pn = c_pn; best_c = i; }
                    else if (c_pn < min_pn2) { min_pn2 = c_pn; }
                }
                else {
                    if (c_dn < min_dn) { min_dn2 = min_dn; min_dn = c_dn; best_c = i; }
                    else if (c_dn < min_dn2) { min_dn2 = c_dn; }
                }
            }

            tt[idx].pn = is_or_node ? min_pn : (int)sum_pn;
            tt[idx].dn = is_or_node ? (int)sum_dn : min_dn;

            if (tt[idx].pn == 0) { // 詰み確定時の手数計算
                if (is_or_node) {
                    int m_len = 999999;
                    for (size_t i = 0; i < tt[idx].moves.size(); ++i) {
                        int ci = tt[idx].child_keys[i] & (DFPN_TT_SIZE - 1);
                        if (tt[ci].key == tt[idx].child_keys[i] && tt[ci].pn == 0) m_len = (std::min)(m_len, tt[ci].mate_length);
                    }
                    tt[idx].mate_length = m_len + 1;
                }
                else {
                    int m_len = -1;
                    for (size_t i = 0; i < tt[idx].moves.size(); ++i) {
                        int ci = tt[idx].child_keys[i] & (DFPN_TT_SIZE - 1);
                        if (tt[ci].key == tt[idx].child_keys[i] && tt[ci].pn == 0) m_len = (std::max)(m_len, tt[ci].mate_length);
                    }
                    tt[idx].mate_length = m_len + 1;
                }
            }

            if (tt[idx].pn >= pn_th || tt[idx].dn >= dn_th || best_c == -1) break;

            int next_pn_th, next_dn_th;
            uint64_t best_key = tt[idx].child_keys[best_c];
            int b_idx = best_key & (DFPN_TT_SIZE - 1);
            int b_pn = (tt[b_idx].key == best_key) ? tt[b_idx].pn : 1;
            int b_dn = (tt[b_idx].key == best_key) ? tt[b_idx].dn : 1;

            if (is_or_node) {
                next_pn_th = (std::min)(pn_th, min_pn2 + 1);
                next_dn_th = (int)(std::min)((long long)DFPN_INF, (long long)dn_th - tt[idx].dn + b_dn);
            }
            else {
                next_pn_th = (int)(std::min)((long long)DFPN_INF, (long long)pn_th - tt[idx].pn + b_pn);
                next_dn_th = (std::min)(dn_th, min_dn2 + 1);
            }


            StateInfo st;

            Move current_move = tt[idx].moves[best_c];

            board.push_move(current_move, st); // コピーした手で進める
            mid(board, !is_or_node, next_pn_th, next_dn_th, depth + 1, path_keys); // ★最後に path_keys を追加！
            board.undo_move(current_move, st);

            
        }
        path_keys.pop_back();
    }
};