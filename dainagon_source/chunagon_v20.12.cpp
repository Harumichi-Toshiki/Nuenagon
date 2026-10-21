#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <fstream> 
#include <unordered_map> 
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include "shogi_utils.h"
#include "search.h"
#include "bitboard.h"
#include "zobrist.h"
#include "vocab.h"

const int INPUT_CHANNELS = 55;

EngineOptions OPTIONS;
int MCTS_TT_SIZE = 131072;
int DFPN_TT_SIZE = 8388608;

class Engine {
private:
    Ort::Env env;
    Ort::SessionOptions session_options;
    Ort::Session* session = nullptr;
    BatchEvaluator* batch_evaluator = nullptr;
    Board board;
    MctsSearcher searcher;

    std::unordered_map<std::string, std::string> book; // 既存のDainagonbook用
    std::unordered_map<uint64_t, std::string> book_db;

    bool book_loaded = false;
    std::vector<std::string> history;

    // スレッド・Ponder管理用フラグ
    std::thread search_thread;
    std::atomic<bool> is_actually_thinking{ false }; // 通常思考(go)中かどうか!!!
    std::atomic<bool> is_pondering_now{ false };     // 先読み(go ponder)中かどうか!

    std::chrono::steady_clock::time_point ponder_start_time;
    //bool ponder_hit_active = false;     // Ponder Hitが発生したかのフラグ
    //int stored_ponder_elapsed = 0;      // 先読みで得した時間(ms)(

public:
    Engine(const std::wstring& model_path)
        : env(ORT_LOGGING_LEVEL_WARNING, "Chunagon") {
        session_options.DisableMemPattern();
        session_options.SetExecutionMode(ORT_SEQUENTIAL);
        session_options.SetIntraOpNumThreads(1);

        // ==========================================
        // ★追加：ONNX Runtimeの計算グラフ最適化を「最大」に設定
        // ==========================================
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        try {
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(session_options, 0));
            std::cout << "info string GPU (DirectML) Init Success! [Device ID: 0]" << std::endl;
        }
        catch (...) {
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(session_options, 1));
            std::cout << "info string GPU (DirectML) Init Success! [Device ID: 1]" << std::endl;
        }
        session = new Ort::Session(env, model_path.c_str(), session_options);
        batch_evaluator = new BatchEvaluator(session);
        searcher.set_evaluator(batch_evaluator);
        board.reset();
    }

    ~Engine() {
        stop_search();
        if (batch_evaluator) delete batch_evaluator;
        if (session) delete session;
    }

    void load_dainagon_txt(const std::string& filename) {
        if (book_loaded) return;
        std::ifstream ifs(filename);
        if (!ifs) return;

        std::string line;
        int count = 0;
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            size_t last_space = line.find_last_of(' ');
            if (last_space == std::string::npos) continue;

            std::string key = line.substr(0, last_space);
            std::string move = line.substr(last_space + 1);
            if (!move.empty() && move.back() == '\r') move.pop_back();

            book[key] = move;
            count++;
        }
        book_loaded = true;
        std::cout << "info string Dainagon Book loaded: " << count << " entries." << std::endl;
    }

    // 478bookのような標準定跡 (.db) のローダー（YANEURAOU-DB2016形式対応）
    void load_yaneura_db(const std::string& filename) {
        if (book_loaded) return;
        std::ifstream ifs(filename);
        if (!ifs) {
            std::cerr << "info string Failed to open .db file: " << filename << std::endl;
            return;
        }

        std::string line;
        Board temp_board;
        int count = 0;

        uint64_t current_z_key = 0;
        bool has_current_sfen = false;
        std::string best_move = "";
        int best_score = -999999;

        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line[0] == '#') continue; // コメント行・ヘッダー行をスキップ

            std::stringstream ss(line);
            std::string token;
            if (!(ss >> token)) continue;

            if (token == "sfen") {
                // 前の局面の最善手を登録
                if (has_current_sfen && !best_move.empty()) {
                    book_db[current_z_key] = best_move;
                    count++;
                }

                // 新しいsfen行を解析
                std::string board_part, turn_part, hand_part, count_part;
                if (!(ss >> board_part >> turn_part >> hand_part >> count_part)) {
                    has_current_sfen = false;
                    continue;
                }

                if (count_part == "-") count_part = "1";
                std::string sfen_str = board_part + " " + turn_part + " " + hand_part + " " + count_part;

                try {
                    temp_board.set_sfen(sfen_str);
                }
                catch (...) {
                    has_current_sfen = false;
                    continue;
                }

                current_z_key = temp_board.get_zobrist_key();
                has_current_sfen = true;
                best_move = "";
                best_score = -999999;
            }
            else if (has_current_sfen) {
                // 候補手行: <手> none <スコア> <depth> <count>
                std::string move_str = token;
                if (move_str == "none" || move_str == "resign") continue;

                std::string second_token;
                int score = 0, depth = 0, visits = 0;
                if (ss >> second_token >> score >> depth >> visits) {
                    // スコアが最高の手を最善手とする
                    if (score > best_score) {
                        best_score = score;
                        best_move = move_str;
                    }
                }
                else {
                    // パース失敗時は最初の手を採用
                    if (best_move.empty()) best_move = move_str;
                }
            }
        }

        // ファイル末尾の最後の局面を登録
        if (has_current_sfen && !best_move.empty()) {
            book_db[current_z_key] = best_move;
            count++;
        }

        book_loaded = true;
        std::cout << "info string DB Book loaded: " << count << " entries." << std::endl;
    }

    // 分岐用の入り口
    void load_book(const std::string& filename) {
        if (book_loaded) return;

        std::ifstream ifs(filename);
        if (!ifs) {
            std::cout << "info string Failed to open book file: " << filename << std::endl;
            return;
        }

        std::string first_line;
        while (std::getline(ifs, first_line)) {
            if (!first_line.empty() && first_line.back() == '\r') first_line.pop_back();
            if (!first_line.empty()) break;
        }
        ifs.close(); // 読み取り後にファイルを閉じる

        if (first_line.empty()) return;

        // YANEURAOU-DB2016形式 または sfen で始まる場合はdbフォーマットとみなす
        if (first_line.find("#YANEURAOU-DB") == 0 || first_line.find("sfen") == 0) {
            std::cout << "info string Detected db file format." << std::endl;
            load_yaneura_db(filename);
        }
        else {
            // Dainagon形式
            load_dainagon_txt(filename);
        }
    }
    

    void new_game() {
        stop_search();
        board.reset();
        history.clear();
        searcher.clear_cache();
        searcher.reset();
    }

    void resize_hash() {
        searcher.resize_hash();
    }

    // ★ Ponder中だろうが通常中だろうが、確実に探索を殺す
    void stop_search() {
        searcher.searching = false;
        if (search_thread.joinable()) search_thread.join();
        is_actually_thinking = false;
        is_pondering_now = false;
    }

    void update_position(const std::string& sfen, const std::vector<std::string>& moves) {
        stop_search(); // 盤面を触る前に確実に探索を止める

        board.set_sfen(sfen);
        // ★修正：historyが空の時（usinewgame直後）に std::equal が誤爆するのを防ぐ
        if (!history.empty() && moves.size() >= history.size() && std::equal(history.begin(), history.end(), moves.begin())) {
            for (size_t i = history.size(); i < moves.size(); ++i) searcher.advance_root(moves[i]);
        }
        else {
            searcher.reset();
        }
        for (const auto& m : moves) board.push_usi(m);
        history = moves;
        board.update_bitboard();
    }

    // ★ GUIから ponderhit が来たときの処理
    void ponderhit() {
        if (is_pondering_now.load()) {
            is_pondering_now = false;
            is_actually_thinking = true;
            searcher.is_ponder_mode = false;

            // ★【最重要】Ponder的中の恩恵：目標時間と最大時間を一気に縮めて即指しさせる
            int t = searcher.current_target_ms.load();
            searcher.current_target_ms.store(t / 4);
            searcher.current_max_ms.store(t / 3);

            std::cout << "info string Ponder hit! Transitioning to real thinking." << std::endl;
        }
    }

    void calculate_thinking_time(const std::string& go_cmd, int& out_target, int& out_max) {
        int time_ms = 0, inc_ms = 0, byoyomi_ms = 0;
        int movetime_ms = 0;
        std::stringstream ss(go_cmd);
        std::string token;
        while (ss >> token) {
            if (board.turn == BLACK) {
                if (token == "btime") ss >> time_ms;
                if (token == "binc") ss >> inc_ms;
            }
            else {
                if (token == "wtime") ss >> time_ms;
                if (token == "winc") ss >> inc_ms;
            }
            if (token == "byoyomi") ss >> byoyomi_ms;
            if (token == "movetime") ss >> movetime_ms;
        }


        if (movetime_ms > 0) {
            // ★修正：固定時間（ベンチマーク）モード。
            // 「自信がある手なら早く切り上げる」という早期打ち切り（Early Stopping）を
            // 無効化するため、目標時間(target)を10倍にして騙し、絶対上限(max)で正確に止める。
            out_target = movetime_ms * 10;
            out_max = movetime_ms;
            return;
        }

        // 1. 基本の考慮時間 (target) を計算
        int divisor = (std::max)(12, 50 - ((int)history.size() / 2));
        out_target = (time_ms / divisor) + (int)(inc_ms * 0.85);

        // ==========================================
        // ★中盤ブーストの適正化（1.2倍に調整）
        // ==========================================
        int current_move = history.size();
        if (current_move >= 50 && current_move <= 70) {
            out_target = (int)(out_target * 2.0);
        }

        // 2. 秒読み(byoyomi)がある場合のマージ
        if (byoyomi_ms > 0) {
            if (time_ms > 0) {
                out_target += byoyomi_ms / 3;
            }
            else {
                out_target = byoyomi_ms * 0.8;
            }
        }

        // 3. 長考の上限を目標時間の「2倍」に設定（溶かしすぎ防止）
        out_max = out_target * 2;

        // 4. 秒読みの絶対限界をオーバーしないためのセーフティ
        if (byoyomi_ms > 0) {
            int byoyomi_limit = (std::max)(100, byoyomi_ms - 500);
            if (out_max > byoyomi_limit && time_ms == 0) {
                out_max = byoyomi_limit;
            }
        }

        // ==========================================
        // ★強力な持ち時間安全装置（スレッド回収遅延・0秒起動対策）
        // ==========================================
        // 64スレッドのjoin待ちを考慮し、安全マージンを2000ms（2秒）に拡大
        int safe = time_ms - OPTIONS.network_delay;

        if (time_ms <= OPTIONS.network_delay && byoyomi_ms == 0) {
            out_target = OPTIONS.min_thinking_time;
            out_max = OPTIONS.min_thinking_time;
        }
        // 通常の残り時間切迫時の制限
        else if (time_ms > 0 && out_max > safe) {
            out_max = safe;
            if (out_target > safe) out_target = safe;

            // 最低でも50msは思考させる（負の値になるのを防ぐ）
            if (out_target < OPTIONS.min_thinking_time) out_target = OPTIONS.min_thinking_time;
            if (out_max < OPTIONS.min_thinking_time) out_max = OPTIONS.min_thinking_time;
        }
    }

    // ★ "go" も "go ponder" もここで受ける
    void go(const std::string& go_cmd_line) {
        bool is_ponder = (go_cmd_line.find("ponder") != std::string::npos);

        if (OPTIONS.own_book && book_loaded && !is_ponder) {
            std::string key = board.to_sfen_key();
            if (book.count(key)) {
                std::cout << "bestmove " << book[key] << std::endl;
                return;
            }
            uint64_t z_key = board.get_zobrist_key();
            if (book_db.count(z_key)) {
                std::cout << "bestmove " << book_db[z_key] << std::endl;
                return;
            }
        }

        int target_ms, max_ms;
        calculate_thinking_time(go_cmd_line, target_ms, max_ms);

        stop_search(); // 古い探索を確実に止める11

        searcher.current_target_ms = target_ms;
        searcher.current_max_ms = max_ms;
        searcher.is_ponder_mode = is_ponder;
        is_actually_thinking = !is_ponder;
        is_pondering_now = is_ponder;

        searcher.searching = true;

        search_thread = std::thread([this]() {
            try {
                // 探索実行
                SearchResult res = searcher.search(board, searcher.current_target_ms, searcher.current_max_ms, searcher.is_ponder_mode);

                // どんな理由で終わっても【必ず】bestmove を返す
                std::cout << "bestmove " << res.best;
                if (!res.ponder.empty()) std::cout << " ponder " << res.ponder;
                std::cout << std::endl;
            }
            catch (const std::exception& e) {
                std::cerr << "info string search_thread exception: " << e.what() << std::endl;
                std::cout << "bestmove resign" << std::endl;
            }
            catch (...) {
                std::cerr << "info string search_thread unknown exception" << std::endl;
                std::cout << "bestmove resign" << std::endl;
            }

            // 状態をリセット
            is_actually_thinking = false;
            is_pondering_now = false;
            });
    }

};


void apply_hash_size(Engine& engine) {
    // 1. ハッシュメモリ(MB)をバイト数に変換
    size_t total_bytes = (size_t)OPTIONS.hash_mb * 1024 * 1024;

    // 2. MCTS(70%) と DF-PN(30%) でメモリを分割
    size_t mcts_bytes = total_bytes * 0.7;
    size_t dfpn_bytes = total_bytes * 0.3;

    // 3. ANDマスク（MCTS_TT_SIZE - 1）計算を壊さないため、2のべき乗（Power of 2）になるよう計算
    MCTS_TT_SIZE = 1024;
    while ((MCTS_TT_SIZE * 2 * sizeof(MctsTTEntry)) <= mcts_bytes) {
        MCTS_TT_SIZE *= 2;
    }

    DFPN_TT_SIZE = 1024;
    while ((DFPN_TT_SIZE * 2 * sizeof(DfpnNode)) <= dfpn_bytes) {
        DFPN_TT_SIZE *= 2;
    }

    engine.resize_hash();
}

int main() {
    //std::wstring model_path = L"C:\\Users\\kadon\\Shogidokoro\\model\\chunagonfp16_vision2.onnx";
    std::wstring model_path = L".\\model\\model.onnx";

    try {
        init_bitboards();
        init_zobrist(); //
        init_vocab();
        auto engine = std::make_unique<Engine>(model_path);

        std::string line;
        // 将棋所（GUI）からの命令を待ち続ける無限ループ
        while (std::getline(std::cin, line)) {
            // ★修正：末尾に '\r' (CR) が残っていれば削除する（Floodgate/CRLF対策）
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line == "quit") {
                break;
            }
            else if (line == "usi") {
                std::cout << "id name Chunagon_v21.2_DirectML" << std::endl;
                std::cout << "id author Harumichi-Toshiki" << std::endl;

                // ==========================================
                // ★追加③-1：GUI（将棋所）に設定項目を教える
                // ==========================================
                std::cout << "option name Threads type spin default 64 min 1 max 128" << std::endl;
                std::cout << "option name USI_Hash type spin default 2048 min 16 max 32768" << std::endl;
                std::cout << "option name USI_Ponder type check default true" << std::endl;
                std::cout << "option name USI_OwnBook type check default true" << std::endl;
                std::cout << "option name BookFile type string default book.txt" << std::endl;
                std::cout << "option name NetworkDelay type spin default 2000 min 0 max 10000" << std::endl;
                std::cout << "option name MinimumThinkingTime type spin default 50 min 0 max 5000" << std::endl;
                std::cout << "option name C_init type string default 1.0" << std::endl;
                std::cout << "option name C_base type string default 8000.0" << std::endl;
                std::cout << "option name NodesLimit type spin default 0 min 0 max 2147483647" << std::endl;
                std::cout << "option name EnteringKingRule type combo default CSARule27 var CSARule27 var NoDeclare" << std::endl;

                std::cout << "usiok" << std::endl;
            }
            // ==========================================
            // ★追加③-2：新しく「setoption」を受信するパーサーを作る
            // ==========================================
            else if (line.find("setoption name ") == 0) {
                std::stringstream ss(line);
                std::string token, name, value;
                ss >> token >> token >> name; // "setoption", "name", "項目名"を読み飛ばす

                size_t val_pos = line.find(" value ");
                if (val_pos != std::string::npos) {
                    value = line.substr(val_pos + 7);
                }

                if (name == "Threads") OPTIONS.threads = std::stoi(value);
                else if (name == "USI_Ponder") OPTIONS.ponder = (value == "true");
                else if (name == "USI_OwnBook") OPTIONS.own_book = (value == "true");
                else if (name == "BookFile") OPTIONS.book_file = value;
                else if (name == "NetworkDelay") OPTIONS.network_delay = std::stoi(value);
                else if (name == "MinimumThinkingTime") OPTIONS.min_thinking_time = std::stoi(value);
                else if (name == "C_init") OPTIONS.c_init = std::stof(value);
                else if (name == "C_base") OPTIONS.c_base = std::stof(value);
                else if (name == "NodesLimit") OPTIONS.nodes_limit = std::stoll(value);
                else if (name == "EnteringKingRule") OPTIONS.entering_king_rule = value;
                else if (name == "USI_Hash") {
                    OPTIONS.hash_mb = std::stoi(value);

                    size_t target_mcts = (OPTIONS.hash_mb * 1024ULL * 1024ULL / 2) / sizeof(MctsTTEntry);
                    size_t target_dfpn = (OPTIONS.hash_mb * 1024ULL * 1024ULL / 2) / sizeof(DfpnNode);

                    // 2のべき乗に切り下げる安全な計算
                    size_t actual_mcts = 1;
                    while (actual_mcts * 2 <= target_mcts) actual_mcts *= 2;
                    MCTS_TT_SIZE = actual_mcts;

                    size_t actual_dfpn = 1;
                    while (actual_dfpn * 2 <= target_dfpn) actual_dfpn *= 2;
                    DFPN_TT_SIZE = actual_dfpn;

                    // 計算後に配列をリサイズ！
                    engine->resize_hash();
                }
            }
            else if (line == "isready") {
                // ★修正：usinewgameより先にisreadyが来た場合、Ponderとハッシュリサイズが競合して即死するのを防ぐ
                engine->stop_search();

                apply_hash_size(*engine);

                if (OPTIONS.own_book) {
                    engine->load_book(OPTIONS.book_file);
                }
                std::cout << "readyok" << std::endl;
            }
            else if (line == "usinewgame") {
                engine->stop_search();
                engine->new_game();
                // ★注意：ここでの2000msスリープはGUIのタイムアウトを引き起こす可能性があるため、削除を推奨します
                // std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            }
            else if (line.find("position") == 0) {
                engine->stop_search();
                std::string sfen = "startpos";
                std::vector<std::string> moves;

                size_t moves_pos = line.find("moves");
                if (line.find("sfen") != std::string::npos) {
                    size_t sfen_start = line.find("sfen") + 5;
                    if (moves_pos != std::string::npos) {
                        sfen = line.substr(sfen_start, moves_pos - sfen_start - 1);
                    }
                    else {
                        sfen = line.substr(sfen_start);
                    }
                }

                if (moves_pos != std::string::npos) {
                    std::stringstream ss(line.substr(moves_pos + 6));
                    std::string m;
                    while (ss >> m) moves.push_back(m);
                }
                // 盤面を更新（ここで履歴の一致を見て、木を引き継ぐか判定します）
                engine->update_position(sfen, moves);
            }
            else if (line.find("go") == 0) {
                engine->go(line);
            }
            else if (line == "stop" || line.find("gameover") == 0) {
                // ★修正：gameover (win/lose/draw) を検知して確実にPonderを止める
                engine->stop_search();
            }
            else if (line == "ponderhit") {
                engine->ponderhit();
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "FATAL ERROR in main: " << e.what() << std::endl;
    }
    return 0;

}