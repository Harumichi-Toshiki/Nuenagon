#pragma once
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <memory>
#include <iostream>
#include <thread>
#include <mutex>
#include <future>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <fstream>
#include <random>
#include <cstdlib>
#include <intrin.h>

#include "shogi_utils.h"
#include "vocab.h"
#include "dfpn.h"
#include <onnxruntime_cxx_api.h>

struct EngineOptions {
    int threads = 64;
    int hash_mb = 2048;
    bool ponder = true;
    bool own_book = true;
    std::string book_file = "book.txt";
    int network_delay = 2000;
    int min_thinking_time = 50;
    float c_base = 8000.0f; // ★修正：正しいデフォルトの数値にする
    float c_init = 1.0f;    // ★修正：正しいデフォルトの数値にする
    long long nodes_limit = 0;
    std::string entering_king_rule = "CSARule27";
};

extern struct EngineOptions OPTIONS;
extern int MCTS_TT_SIZE; // constを外す

const float C_PUCT = 1.0f;
// const int SEARCH_THREADS = 64; // OPTIONS.threads を使うため削除
const int FIXED_BATCH_SIZE = 64;
const float VIRTUAL_LOSS = 0.1f;

inline float get_virtual_loss(int depth) {
    return VIRTUAL_LOSS;
}

inline int get_action_index(const Move& m) {
    int dir_idx = -1;
    if (m.is_drop) {
        // PAWN=1, LANCE=2, KNIGHT=3, SILVER=4, GOLD=5, BISHOP=6, ROOK=7
        // これから1を引けば 0~6 になり、Python側の drop_dict と完全一致する
        dir_idx = 20 + (m.type - 1);
    }
    else {
        int from_x = m.from % 9;
        int from_y = m.from / 9;
        int to_x = m.to % 9;
        int to_y = m.to / 9;
        int dx = to_x - from_x;
        int dy = to_y - from_y;

        if (dy < 0 && dx == 0) dir_idx = 0;
        else if (dy < 0 && dx < 0 && dy == dx) dir_idx = 1;
        else if (dy < 0 && dx > 0 && -dy == dx) dir_idx = 2;
        else if (dy == 0 && dx < 0) dir_idx = 3;
        else if (dy == 0 && dx > 0) dir_idx = 4;
        else if (dy > 0 && dx == 0) dir_idx = 5;
        else if (dy > 0 && dx < 0 && dy == -dx) dir_idx = 6;
        else if (dy > 0 && dx > 0 && dy == dx) dir_idx = 7;
        else if (dy == -2 && dx == -1) dir_idx = 8;
        else if (dy == -2 && dx == 1) dir_idx = 9;

        // 成る場合は +10
        if (m.is_promote) {
            dir_idx += 10;
        }
    }

    // 万が一不正な手なら -1 を返す
    if (dir_idx < 0 || dir_idx > 26) return -1;

    // [方向(27) × マス(81)] の1次元配列のインデックスを返す
    return dir_idx * 81 + m.to;
}

struct SearchResult {
    std::string best;
    std::string ponder;
};

void dump_features(const std::vector<Ort::Float16_t>& features, const std::string& filename) {
    std::ofstream ofs(filename, std::ios::binary);
    if (ofs) {
        ofs.write(reinterpret_cast<const char*>(features.data()), features.size() * sizeof(Ort::Float16_t));
    }
}

inline void atomic_add_float(std::atomic<float>& atom, float val) {
    float current = atom.load(std::memory_order_relaxed);
    while (!atom.compare_exchange_weak(current, current + val, std::memory_order_relaxed));
}

using EvalResult = std::pair<std::shared_ptr<const std::vector<float>>, float>;

struct QueueNode {
    BoardFeatures features;
    EvalResult result;
    bool done{ false };
    std::mutex mtx;
    std::condition_variable cv;
};

#include <cstring>

class BatchEvaluator {
private:
    Ort::Session* session;

    std::mutex queue_mutex;
    std::vector<QueueNode*> queue;

    std::mutex cv_mutex;
    std::condition_variable cv;
    bool running = true;
    std::thread worker;
    const char* input_names[1] = { "input" };
    const char* output_names[2] = { "policy", "value" };

    // Pre-allocated buffers
    std::vector<Ort::Float16_t> input_buffer;
    std::vector<Ort::Float16_t> policy_buffer;
    std::vector<Ort::Float16_t> value_buffer;

    // Pre-allocated tensors wrapping the buffers
    Ort::Value input_tensor{ nullptr };
    Ort::Value output_tensors[2]{ nullptr, nullptr };

    // IO Binding
    std::unique_ptr<Ort::IoBinding> io_binding;

public:
    BatchEvaluator(Ort::Session* sess) : session(sess) {
        input_buffer.resize(FIXED_BATCH_SIZE * 55 * 81, Ort::Float16_t(0.0f));
        policy_buffer.resize(FIXED_BATCH_SIZE * 2187, Ort::Float16_t(0.0f));
        value_buffer.resize(FIXED_BATCH_SIZE, Ort::Float16_t(0.0f));

        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<int64_t> input_shape = { FIXED_BATCH_SIZE, 55, 9, 9 };
        input_tensor = Ort::Value::CreateTensor<Ort::Float16_t>(
            memory_info, input_buffer.data(), input_buffer.size(), input_shape.data(), input_shape.size()
        );

        std::vector<int64_t> policy_shape = { FIXED_BATCH_SIZE, 2187 };
        output_tensors[0] = Ort::Value::CreateTensor<Ort::Float16_t>(
            memory_info, policy_buffer.data(), policy_buffer.size(), policy_shape.data(), policy_shape.size()
        );

        std::vector<int64_t> value_shape = { FIXED_BATCH_SIZE, 1 };
        output_tensors[1] = Ort::Value::CreateTensor<Ort::Float16_t>(
            memory_info, value_buffer.data(), value_buffer.size(), value_shape.data(), value_shape.size()
        );

        io_binding = std::make_unique<Ort::IoBinding>(*session);
        io_binding->BindInput("input", input_tensor);
        io_binding->BindOutput("policy", output_tensors[0]);
        io_binding->BindOutput("value", output_tensors[1]);

        worker = std::thread(&BatchEvaluator::loop, this);
    }
    ~BatchEvaluator() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            running = false;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    EvalResult predict_async(const BoardFeatures& features, QueueNode& stack_node) {
        stack_node.features = features;
        {
            std::lock_guard<std::mutex> lock(stack_node.mtx);
            stack_node.done = false;
            std::atomic_thread_fence(std::memory_order_release);
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue.push_back(&stack_node);
        }
        cv.notify_one();

        {
            std::unique_lock<std::mutex> lock(stack_node.mtx);
            stack_node.cv.wait(lock, [&stack_node] { return stack_node.done; });
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        return stack_node.result;
    }

private:
    void loop() {
        std::vector<QueueNode*> local_queue;
        std::vector<QueueNode*> batch;
        batch.reserve(FIXED_BATCH_SIZE);

        while (running) {
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                cv.wait(lock, [this] {
                    return !running || !queue.empty();
                    });

                if (!running && queue.empty()) break;

                // ==========================================
                // ★ 復活：Dynamic Batching（バッチ溜め）
                // DirectMLの推論特性に合わせ、待機時間を 500マイクロ秒 (0.5ms) に設定。
                // これにより、NPSのボトルネックが解消されます！
                // ==========================================
                // yield() によるアクティブスピンロックを廃止し、cv.wait_for を使用します。
// 評価スレッドはミューテックスを手放して休眠し、キューが64個埋まるか500us経過した時に自動で起床します。
                if (queue.size() < FIXED_BATCH_SIZE) {
                    cv.wait_for(lock, std::chrono::microseconds(500), [this] {
                        return !running || queue.size() >= FIXED_BATCH_SIZE;
                        });
                }
                // ==========================================

                if (!queue.empty()) {
                    local_queue.swap(queue);
                }
            }

            if (local_queue.empty()) continue;

            size_t total_items = local_queue.size();
            size_t processed = 0;

            while (processed < total_items) {
                batch.clear();
                size_t actual_bs = (std::min)(total_items - processed, (size_t)FIXED_BATCH_SIZE);
                batch.assign(local_queue.begin() + processed, local_queue.begin() + processed + actual_bs);
                processed += actual_bs;

                try {
                    // Copy to pre-allocated input_buffer directly
                    Ort::Float16_t* input_ptr = input_buffer.data();
                    for (size_t i = 0; i < actual_bs; ++i) {
                        std::memcpy(input_ptr + i * 55 * 81, batch[i]->features.data, 55 * 81 * sizeof(Ort::Float16_t));
                    }
                    if (actual_bs < FIXED_BATCH_SIZE) {
                        std::memset(input_ptr + actual_bs * 55 * 81, 0, (FIXED_BATCH_SIZE - actual_bs) * 55 * 81 * sizeof(Ort::Float16_t));
                    }

                    io_binding->BindInput("input", input_tensor);
                    io_binding->BindOutput("policy", output_tensors[0]);
                    io_binding->BindOutput("value", output_tensors[1]);

                    session->Run(
                        Ort::RunOptions{ nullptr }, *io_binding
                    );

                    size_t policy_dim = 2187;

                    for (size_t i = 0; i < actual_bs; ++i) {
                        std::vector<float> p(policy_dim);
                        for (size_t j = 0; j < policy_dim; ++j) {
                            p[j] = policy_buffer[i * policy_dim + j].ToFloat();
                        }

                        float max_logit = -999999.0f;
                        for (float val : p) {
                            if (val > max_logit) max_logit = val;
                        }
                        float sum_exp = 0.0f;
                        for (float& val : p) {
                            val = std::exp(val - max_logit);
                            sum_exp += val;
                        }
                        for (float& val : p) {
                            val /= sum_exp;
                        }

                        auto p_ptr = std::make_shared<const std::vector<float>>(std::move(p));
                        batch[i]->result = { p_ptr, value_buffer[i].ToFloat() };

                        {
                            std::lock_guard<std::mutex> lock(batch[i]->mtx);
                            batch[i]->done = true;
                            std::atomic_thread_fence(std::memory_order_release);
                            batch[i]->cv.notify_one();
                        }
                    }
                }
                catch (const std::exception& e) {
                    std::cerr << "info string evaluator std exception: " << e.what() << std::endl;
                    for (auto& req : batch) {
                        req->result = { nullptr, 0.0f };
                        {
                            std::lock_guard<std::mutex> lock(req->mtx);
                            req->done = true;
                            std::atomic_thread_fence(std::memory_order_release);
                            req->cv.notify_one();
                        }
                    }
                }
                catch (...) {
                    std::cerr << "info string evaluator unknown exception" << std::endl;
                    for (auto& req : batch) {
                        req->result = { nullptr, 0.0f };
                        {
                            std::lock_guard<std::mutex> lock(req->mtx);
                            req->done = true;
                            std::atomic_thread_fence(std::memory_order_release);
                            req->cv.notify_one();
                        }
                    }
                }
            }

            local_queue.clear();
        }

        // Cleanup remaining nodes if any
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            for (auto* node : queue) {
                node->result = { nullptr, 0.0f };
                {
                    std::lock_guard<std::mutex> lock(node->mtx);
                    node->done = true;
                    std::atomic_thread_fence(std::memory_order_release);
                    node->cv.notify_one();
                }
            }
            queue.clear();
        }
    }
};

struct alignas(64) Node {
    std::atomic<int> visit_count{ 0 };
    std::atomic<float> value_sum{ 0.0f };

    std::atomic<float> policy_prob{ 0.0f };

    float raw_value = 0.5f;
    Move move_obj;
    std::atomic<bool> is_expanded{ false };
    std::atomic<bool> is_evaluating{ false };
    std::atomic<bool> has_noise{ false };

    Node* first_child = nullptr;
    Node* sibling = nullptr;

    Node() {}

    Node(float p, Move m) : move_obj(m) {
        policy_prob.store(p, std::memory_order_relaxed);
    }
    ~Node() {}

    void init(float p, Move m) {
        visit_count.store(0, std::memory_order_relaxed);
        value_sum.store(0.0f, std::memory_order_relaxed);
        policy_prob.store(p, std::memory_order_relaxed);

        raw_value = 0.5f;
        move_obj = m;
        is_expanded.store(false, std::memory_order_relaxed);
        is_evaluating.store(false, std::memory_order_relaxed);
        has_noise.store(false, std::memory_order_relaxed);

        first_child = nullptr;
        sibling = nullptr;
    }

    float get_score(int total_parent_visits, float fpu_value) {
        int n = visit_count.load(std::memory_order_relaxed);
        float v = value_sum.load(std::memory_order_relaxed);
        float q = (n == 0) ? fpu_value : (v / n);

        float c_base = OPTIONS.c_base;
        float c_init = OPTIONS.c_init;
        float dynamic_c_puct = c_init + std::log((total_parent_visits + c_base + 1.0f) / c_base);

        float p = policy_prob.load(std::memory_order_relaxed);
        float puct = dynamic_c_puct * p * (std::sqrt((float)total_parent_visits) / (1 + n));
        return q + puct;
    }
};

class NodeAllocator {
private:
    static constexpr size_t CHUNK_SIZE = 1048576; // 1M nodes per chunk
    static constexpr size_t MAX_CHUNKS = 256;     // Max 256M nodes total (16GB max)

    std::atomic<Node*> chunks[MAX_CHUNKS];
    std::atomic<size_t> counter{ 0 };
    std::mutex allocation_mtx;

public:
    NodeAllocator() {
        for (size_t i = 0; i < MAX_CHUNKS; ++i) {
            chunks[i].store(nullptr, std::memory_order_relaxed);
        }
        // Pre-allocate the first chunk to avoid any allocation during first search
        allocate_chunk(0);
    }

    ~NodeAllocator() {
        for (size_t i = 0; i < MAX_CHUNKS; ++i) {
            Node* chunk = chunks[i].load(std::memory_order_relaxed);
            if (chunk) {
                for (size_t j = 0; j < CHUNK_SIZE; ++j) {
                    chunk[j].~Node();
                }
#ifdef _MSC_VER
                _aligned_free(chunk);
#else
                std::free(chunk);
#endif
            }
        }
    }

    void allocate_chunk(size_t chunk_idx) {
        std::lock_guard<std::mutex> lock(allocation_mtx);
        if (chunks[chunk_idx].load(std::memory_order_relaxed) == nullptr) {
            Node* ptr;
#ifdef _MSC_VER
            ptr = (Node*)_aligned_malloc(CHUNK_SIZE * sizeof(Node), 64);
#else
            ptr = (Node*)std::aligned_alloc(64, CHUNK_SIZE * sizeof(Node));
#endif
            if (ptr == nullptr) {
                std::cerr << "FATAL ERROR: NodeAllocator failed to allocate chunk!" << std::endl;
                std::exit(EXIT_FAILURE);
            }
            for (size_t i = 0; i < CHUNK_SIZE; ++i) {
                new (&ptr[i]) Node();
            }
            chunks[chunk_idx].store(ptr, std::memory_order_release);
        }
    }

    Node* alloc(float p, Move m) {
        size_t idx = counter.fetch_add(1, std::memory_order_relaxed);
        size_t chunk_idx = idx / CHUNK_SIZE;
        size_t offset = idx % CHUNK_SIZE;

        if (chunk_idx >= MAX_CHUNKS) [[unlikely]] {
            std::cerr << "FATAL ERROR: NodeAllocator reached maximum capacity of 256M nodes!" << std::endl;
            std::exit(EXIT_FAILURE);
        }

        Node* chunk = chunks[chunk_idx].load(std::memory_order_acquire);
        if (chunk == nullptr) [[unlikely]] {
            allocate_chunk(chunk_idx);
            chunk = chunks[chunk_idx].load(std::memory_order_acquire);
        }

        Node* n = &chunk[offset];
        n->init(p, m);
        return n;
    }

    void reset() {
        // Reuse allocated chunks by resetting the counter. $O(1)$ reset!
        counter.store(0, std::memory_order_relaxed);
    }
};

struct alignas(64) CacheLock {
    std::atomic_flag flag;
    CacheLock() { flag.clear(); }
    CacheLock(const CacheLock&) = delete;
    CacheLock& operator=(const CacheLock&) = delete;
};

struct MctsTTEntry {
    uint64_t key = 0;
    float value = 0.0f;
    std::shared_ptr<const std::vector<float>> policy;
};

class ThreadGuard {
    std::thread& t;
public:
    explicit ThreadGuard(std::thread& t_) : t(t_) {}
    ~ThreadGuard() {
        if (t.joinable()) {
            t.join();
        }
    }
    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;
};

class MctsSearcher {
public:
    static constexpr int MAX_SIM_DEPTH = 1024;
    Node* root = nullptr;
    BatchEvaluator* evaluator = nullptr;
    std::atomic<bool> searching{ false };
    std::chrono::steady_clock::time_point start_time;
    int time_limit_ms = 0;
    int initial_visits = 0;
    std::atomic<int> current_target_ms{ 0 };
    std::atomic<int> current_max_ms{ 0 };
    std::atomic<bool> is_ponder_mode{ false };

    std::vector<MctsTTEntry> nn_cache;
    static constexpr int CACHE_LOCK_SIZE = 65536;
    CacheLock cache_locks[CACHE_LOCK_SIZE];
    DfpnSearcher* dfpn = nullptr;

    DfpnSearcher* anti_dfpn = nullptr;
    std::atomic<bool> anti_searching{ true };

    NodeAllocator allocators[2];
    int current_alloc_idx = 0;

    NodeAllocator& current_alloc() { return allocators[current_alloc_idx]; }
    NodeAllocator& next_alloc() { return allocators[1 - current_alloc_idx]; }

    MctsSearcher() {
        nn_cache.resize(MCTS_TT_SIZE);
        for (int i = 0; i < CACHE_LOCK_SIZE; ++i) cache_locks[i].flag.clear();
        dfpn = new DfpnSearcher(&searching);
        anti_dfpn = new DfpnSearcher(&anti_searching);
    }
    ~MctsSearcher() {
        if (dfpn) delete dfpn;
    }

    void set_evaluator(BatchEvaluator* ev) { evaluator = ev; }

    void clear_cache() {
        // ==========================================
        // ★修正: MCTSの評価値キャッシュは「あえて」クリアしない！
        // 1. 何百万回もの shared_ptr の破棄による1分間のフリーズを消滅させる
        // 2. 前局で計算した評価値を次局の序盤（定跡）としてそのまま再利用する
        // ==========================================

        // DF-PNのクリアだけは残しておく（詰み探索のバグ防止のため）
        if (dfpn) dfpn->clear();
        if (anti_dfpn) anti_dfpn->clear();
    }

    void resize_hash() {
        nn_cache.clear();
        nn_cache.resize(MCTS_TT_SIZE);
        for (int i = 0; i < CACHE_LOCK_SIZE; ++i) cache_locks[i].flag.clear();

        // DF-PN側にもリサイズを伝播させる
        if (dfpn) dfpn->resize_hash();
        if (anti_dfpn) anti_dfpn->resize_hash();
    }

    void reset() {
        current_alloc().reset();
        Move null_move = { -1, -1, 0, false, false };
        root = current_alloc().alloc(1.0f, null_move);
    }

    Node* clone_tree(Node* old_node, NodeAllocator& target_alloc) {
        if (!old_node) return nullptr;
        Node* new_n = target_alloc.alloc(old_node->policy_prob, old_node->move_obj);

        int visits = old_node->visit_count.load(std::memory_order_relaxed);
        new_n->visit_count.store(visits, std::memory_order_relaxed);
        new_n->value_sum.store(old_node->value_sum.load(std::memory_order_relaxed), std::memory_order_relaxed);
        new_n->raw_value = old_node->raw_value;
        new_n->has_noise.store(old_node->has_noise.load(std::memory_order_relaxed), std::memory_order_relaxed);

        // ★ ここがNPS低下を救う究極のガベージコレクション
        // 訪問回数が「1回以下（＝一度展開されただけで深く読まれていない）」のノードは、
        // 80個以上ある子供ノードをコピーせず、ただの「未展開の葉ノード」に戻します。
        // これにより、強さに直結する本筋の読みだけを残し、消費メモリを90%以上削減します。
        if (visits < 2) {
            new_n->is_expanded.store(false, std::memory_order_relaxed);
            new_n->is_evaluating.store(false, std::memory_order_relaxed);
            new_n->first_child = nullptr;
            return new_n;
        }

        new_n->is_expanded.store(old_node->is_expanded.load(std::memory_order_relaxed), std::memory_order_relaxed);
        new_n->is_evaluating.store(false, std::memory_order_relaxed);

        Node* curr_old_child = old_node->first_child;
        Node* prev_new_child = nullptr;

        while (curr_old_child != nullptr) {
            Node* new_child = clone_tree(curr_old_child, target_alloc);
            if (new_child != nullptr) {
                if (prev_new_child == nullptr) {
                    new_n->first_child = new_child;
                }
                else {
                    prev_new_child->sibling = new_child;
                }
                prev_new_child = new_child;
            }
            curr_old_child = curr_old_child->sibling;
        }
        return new_n;
    }

    void advance_root(const std::string& move_usi) {
        if (!root) {
            reset();
            return;
        }
        Node* next_root = nullptr;
        for (Node* c = root->first_child; c != nullptr; c = c->sibling) {
            if (move_to_usi(c->move_obj) == move_usi) {
                next_root = c;
                break;
            }
        }

        if (next_root) {
            NodeAllocator& target = next_alloc();
            target.reset();
            root = clone_tree(next_root, target);
            if (root) {
                root->has_noise.store(false, std::memory_order_relaxed);
            }

            current_alloc_idx = 1 - current_alloc_idx;
            next_alloc().reset();
        }
        else {
            reset();
        }
    }


    void print_info() {
        // ★修正: rootが完全に展開されているか(is_expanded)をアトミックに確認
        if (!root || root->visit_count == 0 || !root->is_expanded.load(std::memory_order_acquire)) return;

        std::string pv_str = "";
        Node* curr = root;
        int depth = 0;

        // ★修正: currが展開済みであることを確認してから子ノードを辿る
        while (curr && curr->is_expanded.load(std::memory_order_acquire) && curr->first_child != nullptr) {
            if (depth > 30) break;
            Node* best = nullptr;
            int max_n = -1;
            for (Node* c = curr->first_child; c != nullptr; c = c->sibling) {
                // ★修正: visit_countへのアクセスも明示的にロード
                int v = c->visit_count.load(std::memory_order_relaxed);
                if (v > max_n) { max_n = v; best = c; }
            }
            if (!best || best->visit_count.load(std::memory_order_relaxed) == 0) break;
            pv_str += move_to_usi(best->move_obj) + " ";
            curr = best;
            depth++;
        }

        Node* best_child = nullptr;
        int max_visits = -1;
        // ★修正: rootの展開確認
        if (root->is_expanded.load(std::memory_order_acquire)) {
            for (Node* child = root->first_child; child != nullptr; child = child->sibling) {
                int v = child->visit_count.load(std::memory_order_relaxed);
                if (v > max_visits) {
                    max_visits = v;
                    best_child = child;
                }
            }
        }

        if (best_child) {
            // ★修正: value_sumもアトミックにロード
            float win_rate = best_child->value_sum.load(std::memory_order_relaxed) / (float)max_visits;
            win_rate = (std::max)(0.0001f, (std::min)(0.9999f, win_rate));
            int score = int(-800.0f * std::log(1.0f / win_rate - 1.0f));

            long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
            long long added_nodes = root->visit_count.load(std::memory_order_relaxed) - initial_visits;
            long long nps = (elapsed > 0) ? (added_nodes * 1000 / elapsed) : 0;

            std::cout << "info depth " << depth << " nodes " << root->visit_count.load(std::memory_order_relaxed)
                << " nps " << nps << " score cp " << score
                << " time " << elapsed << " pv " << pv_str << std::endl;
        }
    }

    void add_dirichlet_noise(Node* root_node) {
        if (!root_node || root_node->first_child == nullptr) return;

        float alpha = 0.15f;
        float epsilon = 0.25f;

        std::mt19937 rng(std::random_device{}());
        std::gamma_distribution<float> gamma(alpha, 1.0f);

        int child_count = 0;
        for (Node* c = root_node->first_child; c != nullptr; c = c->sibling) child_count++;

        float noise[MAX_LEGAL_MOVES];
        float noise_sum = 0.0f;
        for (int i = 0; i < child_count; ++i) {
            float n = gamma(rng);
            noise[i] = n;
            noise_sum += n;
        }

        // ゼロ除算対策: ノイズの合計が極端に小さい場合は付与をキャンセル
        if (noise_sum < 1e-5f) return;

        int i = 0;
        for (Node* c = root_node->first_child; c != nullptr; c = c->sibling) {
            float n = noise[i] / noise_sum;
            float current_p = c->policy_prob.load(std::memory_order_relaxed);
            float new_p = (1.0f - epsilon) * current_p + epsilon * n;
            c->policy_prob.store(new_p, std::memory_order_relaxed);
            i++;
        }
    }



    SearchResult search(Board current_board, int target_ms, int max_ms, bool is_ponder) {
        // ★修正：ここで `searching = true;` と上書きしてはいけない
        // （メインスレッドの stop_search() による false の書き込みを潰してしまい、デッドロックになるため削除）

        is_ponder_mode = is_ponder;
        start_time = std::chrono::steady_clock::now();

        // ==========================================
        // ★追加：探索を開始する前に、入玉宣言が可能かチェックする
        // ==========================================
        if (!is_ponder && OPTIONS.entering_king_rule == "CSARule27" && current_board.can_declare_win()) {
            std::cout << "info string Declare Win (27/28 points rule)!" << std::endl;
            searching = false;
            return { "win", "" }; // USIプロトコルの勝利宣言コマンド "win" を返す
        }

        // 1. 合法手をスタック配列に生成
        Move root_moves[MAX_LEGAL_MOVES];
        Move* root_end = generate_moves(current_board.bit_pos, root_moves);

        // 合法手がない場合は投了
        if (root_moves == root_end) {
            std::cout << "info string I have no legal moves. Resigning." << std::endl;

            // ★追加：Ponder中の場合は、GUIから指示（stop または ponderhit）が来るまで待機する
            while (searching.load()) {
                if (!is_ponder_mode.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            return { "resign", "" };
        }

        // 2. 1手詰み（即詰み）チェックのループ（ポインタ方式）
        for (Move* m = root_moves; m != root_end; ++m) {
            StateInfo st;
            current_board.push_move(*m, st);

            Move next_moves[MAX_LEGAL_MOVES];
            // 相手に合法手がない（＝詰み）かチェック
            if (generate_moves(current_board.bit_pos, next_moves) == next_moves) {
                std::string usi_str = move_to_usi(*m);
                std::cout << "info depth 1 score mate 1 pv " << usi_str << std::endl;

                current_board.undo_move(*m, st); // 忘れずに戻す

                while (searching.load()) {
                    if (!is_ponder_mode.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                return { usi_str, "" };
            }
            current_board.undo_move(*m, st);
        }

        Move null_move = { -1, -1, 0, false, false };
        if (!root) root = current_alloc().alloc(1.0f, null_move);
        initial_visits = root->visit_count.load();
        time_limit_ms = max_ms;

        // 3. ルートノードの初期展開
        if (!root->is_expanded.load()) {
            BoardFeatures features;
            make_features(current_board, features);
            QueueNode stack_node;
            auto result = evaluator->predict_async(features, stack_node);

            // ★ 引数を5つ（root_endを含む）渡す
            expand_node_with_moves(root, current_board, root_moves, root_end, result.first);

            root->raw_value = result.second;
            root->is_expanded.store(true);
        }

        bool expected = false;
        if (!is_ponder_mode.load() && root->has_noise.compare_exchange_strong(expected, true)) {
            add_dirichlet_noise(root);
        }

        std::vector<std::thread> threads;
        for (int i = 0; i < OPTIONS.threads; ++i) {
            threads.emplace_back(&MctsSearcher::worker, this, current_board);
        }

        std::string mate_move = "";
        std::thread dfpn_thread([&]() {
            try {
                if (!current_board.in_check(current_board.turn)) {
                    if (dfpn->search_mate(current_board)) {
                        mate_move = dfpn->get_best_mate();
                        if (!is_ponder_mode.load()) {
                            searching = false;
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                std::cerr << "info string dfpn_thread exception: " << e.what() << std::endl;
            }
            catch (...) {
                std::cerr << "info string dfpn_thread unknown exception" << std::endl;
            }
            });

        // ★追加：この1行を入れるだけで、search()関数をどうやって抜けても確実にスレッドが安全に終了します
        ThreadGuard dfpn_guard(dfpn_thread);

        auto last_print_time = start_time;

        while (searching) {
            bool expected_noise = false;
            if (!is_ponder_mode.load() && root->has_noise.compare_exchange_strong(expected_noise, true)) {
                add_dirichlet_noise(root);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            auto now = std::chrono::steady_clock::now();
            auto elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

            if (OPTIONS.nodes_limit > 0 && root->visit_count.load(std::memory_order_relaxed) >= OPTIONS.nodes_limit) {
                searching = false; break;
            }

            if (!is_ponder_mode.load()) {
                int max_limit = current_max_ms.load();
                if (elapsed >= max_limit) { searching = false; break; }

                int n1 = 0, n2 = 0;
                for (Node* c = root->first_child; c != nullptr; c = c->sibling) {
                    int v = c->visit_count.load(std::memory_order_relaxed);
                    if (v > n1) { n2 = n1; n1 = v; }
                    else if (v > n2) { n2 = v; }
                }

                if (n1 > 0) {
                    // 1. Mathematical Early Stopping (cannot be overtaken)
                    long long visited_nodes = root->visit_count.load(std::memory_order_relaxed) - initial_visits;
                    long long nps = (elapsed > 10) ? (visited_nodes * 1000 / elapsed) : 0;
                    long long rem_ms = max_limit - elapsed;
                    long long rem_visits = (nps * rem_ms) / 1000;

                    if (rem_ms > 0 && nps > 0 && n1 > n2 + rem_visits) {
                        searching = false;
                        break;
                    }

                    // 2. Dynamic Target Stopping & Extension
                    float t = (float)current_target_ms.load();
                    if (t < 500) {
                        if (elapsed >= t) { searching = false; break; }
                    }
                    else {
                        bool is_close = (n1 <= n2 * 1.08f); // 8% difference -> highly competitive choice

                        if (is_close) {
                            // 勝負手：1番手と2番手が拮抗している場合は、最大時間(max_limit)まで探索を延長する
                        }
                        else {
                            if (elapsed >= t * 0.5f && n1 >= n2 * 1.5f) { searching = false; break; }
                            if (elapsed >= t * 0.9f && n1 >= n2 * 1.2f) { searching = false; break; }
                            if (elapsed >= t * 1.1f && n1 >= n2 * 1.05f) { searching = false; break; }
                            if (elapsed >= t * 1.5f) { searching = false; break; }
                        }
                    }
                }
            }

            auto ms_since_print = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print_time).count();
            if (ms_since_print >= 500) {
                print_info();
                last_print_time = now;
            }
        }
        print_info();

        for (auto& t : threads) t.join();

        searching = false;
        if (dfpn_thread.joinable()) dfpn_thread.join();

        if (!mate_move.empty()) {
            std::vector<std::string> pv = dfpn->get_mate_pv(current_board);
            std::string pv_str = "";
            for (const auto& m : pv) pv_str += m + " ";
            int mate_ply = pv.empty() ? 1 : pv.size();
            std::cout << "info depth " << mate_ply << " score mate " << mate_ply << " pv " << pv_str << std::endl;
            return { mate_move, "" };
        }

        Node* b1 = nullptr;

        if (root->first_child != nullptr) {
            // 1. 候補手を「訪問回数順」に並べる
            std::vector<Node*> candidates;
            for (Node* c = root->first_child; c != nullptr; c = c->sibling) {
                candidates.push_back(c);
            }
            std::sort(candidates.begin(), candidates.end(), [](Node* a, Node* b) {
                return a->visit_count.load() > b->visit_count.load();
                });

            // 2. 上位3手について「指した瞬間に自玉が詰まないか」をDF-PNで検問
            for (size_t i = 0; i < candidates.size(); ++i) {
                Node* top_node = candidates[i];
                Board next_board = current_board;
                StateInfo st;
                next_board.push_move(top_node->move_obj, st); // 候補手を指してみる

                anti_searching.store(true);
                anti_dfpn->set_max_nodes(50000); // 0.05秒程度で切り上げる制限

                if (anti_dfpn->search_mate(next_board)) {
                    // 💥 詰み発見！この手は危ないので、次の候補へ
                    std::cout << "info string Anti-Mate! " << move_to_usi(top_node->move_obj) << " is dangerous." << std::endl;
                    continue;
                }
                else {
                    // ✅ 安全確認完了。これを最終手とする
                    b1 = top_node;
                    break;
                }
            }
            // 全候補が全滅（絶望的状況）なら、仕方ないので1番手を選ぶ
            if (b1 == nullptr) b1 = candidates[0];
        }

        // 3. Ponder（先読み）用の相手の予測手を取得して返す（既存の機能を維持）
        std::string best_move = b1 ? move_to_usi(b1->move_obj) : "resign";
        std::string ponder_move = "";
        if (b1) {
            int max_v2 = -1;
            for (Node* c2 = b1->first_child; c2 != nullptr; c2 = c2->sibling) {
                int v2 = c2->visit_count.load();
                if (v2 > max_v2) { max_v2 = v2; ponder_move = move_to_usi(c2->move_obj); }
            }
        }
        return { best_move, ponder_move };
    }

private:
    void worker(Board root_board) {
        int iter_count = 0;
        BoardFeatures features_buffer;
        try {
            while (searching) {
                if ((++iter_count & 63) == 0) [[unlikely]] {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
                    if (!is_ponder_mode.load() && elapsed >= current_max_ms.load()) {
                        searching = false; break;
                    }
                }
                run_simulation(root_board, features_buffer);
            }
        }
        catch (const std::exception& e) {
            // メモリ不足等の例外を捕捉。エンジンを落とさず、安全に探索を打ち切る
            std::cerr << "info string worker thread exception: " << e.what() << std::endl;
            // 異常事態なので、他のスレッドにも探索終了を通知
            searching = false;
        }
        catch (...) {
            std::cerr << "info string worker thread unknown exception" << std::endl;
            searching = false;
        }
    }

    void revert_virtual_loss(Node* path[], int path_size) {
        for (int i = path_size - 1; i >= 1; --i) {
            Node* n = path[i];
            float vl_val = get_virtual_loss(i);
            n->visit_count.fetch_sub(1, std::memory_order_relaxed);
            atomic_add_float(n->value_sum, vl_val);
        }
    }

    void run_simulation(Board sim_board, BoardFeatures& features_buffer) {
        Node* node = root;
        Node* path[MAX_SIM_DEPTH];
        int path_size = 0;
        path[path_size++] = node;

        int loop_count = 0;
        uint64_t sim_history[MAX_SIM_DEPTH];
        int sim_history_size = 0;
        sim_history[sim_history_size++] = sim_board.get_zobrist_key();

        while (true) {
            if (loop_count++ > 200 || path_size >= (MAX_SIM_DEPTH - 4)) [[unlikely]] break;

            if (!node->is_expanded.load(std::memory_order_acquire)) {
                bool expected = false;
                if (node->is_evaluating.compare_exchange_strong(expected, true)) {
                    break;
                }
                else {
                    revert_virtual_loss(path, path_size);
                    return;
                }
            }

            if (node->first_child == nullptr) [[unlikely]] break;

            Node* best_child = nullptr;
            float best_score = -999999.0f;

            int parent_visits = node->visit_count.load(std::memory_order_relaxed);

            // ★修正：FPU（未探索ノードの期待値）の反転バグを完全修正
            float q_val;
            if (parent_visits > 0) {
                // value_sum には「相手の勝率」が蓄積されているため、1.0から引いて「自分の勝率」に戻す！
                q_val = 1.0f - (node->value_sum.load(std::memory_order_relaxed) / (float)parent_visits);
            }
            else {
                // raw_value は初期評価（自分の勝率）なのでそのまま使う
                q_val = node->raw_value;
            }

            float parent_q = (std::max)(0.0f, (std::min)(1.0f, q_val));
            float fpu_val = parent_q - 0.05f;

            for (Node* child = node->first_child; child != nullptr; child = child->sibling) {
                float score = child->get_score(parent_visits, fpu_val);

                if (!child->is_expanded.load(std::memory_order_relaxed) && child->is_evaluating.load(std::memory_order_relaxed)) {
                    score -= 1000.0f;
                }

                if (score > best_score) { best_score = score; best_child = child; }
            }

            if (best_child) [[likely]] {
                best_child->visit_count.fetch_add(1, std::memory_order_relaxed);
                atomic_add_float(best_child->value_sum, -get_virtual_loss(path_size));

                node = best_child;
                StateInfo st;
                sim_board.push_move(node->move_obj, st);
                path[path_size++] = node;

                uint64_t current_key = sim_board.get_zobrist_key();
                bool found = false;
                for (int i = 0; i < sim_history_size; ++i) {
                    if (sim_history[i] == current_key) {
                        found = true;
                        break;
                    }
                }
                if (found) [[unlikely]] {
                    bool is_checking = sim_board.in_check(sim_board.turn);
                    float loop_value = is_checking ? 0.0f : 0.5f;
                    float value = 1.0f - loop_value;

                    for (int i = path_size - 1; i >= 0; --i) {
                        Node* n = path[i];
                        if (i != 0) {
                            atomic_add_float(n->value_sum, get_virtual_loss(i) + value);
                        }
                        else {
                            n->visit_count.fetch_add(1, std::memory_order_relaxed);
                            atomic_add_float(n->value_sum, value);
                        }
                        value = 1.0f - value;
                    }
                    return;
                }
                sim_history[sim_history_size++] = current_key;
            }
            else {
                break;
            }
        }

        float value = 0.0f;
        bool should_backprop = false;

        if (loop_count > 200) {
            value = 0.5f;
            should_backprop = true;
        }
        else if (!node->is_expanded.load() && node->is_evaluating.load()) {
            Move l_moves[MAX_LEGAL_MOVES];
            Move* end_moves = generate_moves(sim_board.bit_pos, l_moves);

            if (l_moves == end_moves) {
                node->is_expanded.store(true, std::memory_order_release);
                node->is_evaluating.store(false, std::memory_order_release);
                value = 0.0f;
                should_backprop = true;
            }
            else {
                uint64_t z_key = sim_board.get_zobrist_key();
                if (sim_board.last_to_sq >= 0 && sim_board.last_to_sq < 81) {
                    z_key ^= ZOBRIST_LAST_TO[sim_board.last_to_sq];
                }
                int idx = z_key & (MCTS_TT_SIZE - 1);
                int lock_idx = idx & (CACHE_LOCK_SIZE - 1);

                bool cache_hit = false;
                std::shared_ptr<const std::vector<float>> cached_policy;
                float cached_value = 0.0f;
                bool in_flight = false;

                // 1. Check cache and reservation
                {
                    while (cache_locks[lock_idx].flag.test_and_set(std::memory_order_acquire)) {
                        _mm_pause();
                    }

                    if (nn_cache[idx].key == z_key) {
                        if (nn_cache[idx].policy != nullptr) {
                            cached_policy = nn_cache[idx].policy;
                            cached_value = nn_cache[idx].value;
                            cache_hit = true;
                        }
                        else {
                            in_flight = true;
                        }
                    }
                    else {
                        // Empty slot or different key: reserve slot by marking it as in-flight
                        nn_cache[idx].key = z_key;
                        nn_cache[idx].policy = nullptr;
                        nn_cache[idx].value = 0.0f;
                    }
                    cache_locks[lock_idx].flag.clear(std::memory_order_release);
                }

                // 2. If already evaluating by another thread, sleep and wait for completion
                if (in_flight) {
                    // Windowsの sleep_for による15msの致命的なフリーズを避けるため、
                    // 待機せずに自スレッドでも重複して評価を実行します。
                    in_flight = false;
                }

                // 3. Process cache hit or perform evaluation
                if (cache_hit) {
                    if (!node->is_expanded.load()) {
                        expand_node_with_moves(node, sim_board, l_moves, end_moves, cached_policy);
                        node->raw_value = cached_value;
                        node->is_expanded.store(true, std::memory_order_release);
                    }
                    node->is_evaluating.store(false, std::memory_order_release);
                    value = cached_value;
                    should_backprop = true;
                }
                else {
                    make_features(sim_board, features_buffer);
                    EvalResult result;
                    try {
                        QueueNode stack_node;
                        result = evaluator->predict_async(features_buffer, stack_node);
                    }
                    catch (const std::exception&) {
                        // Release reservation on failure
                        {
                            while (cache_locks[lock_idx].flag.test_and_set(std::memory_order_acquire)) {
                                _mm_pause();
                            }
                            if (nn_cache[idx].key == z_key && nn_cache[idx].policy == nullptr) {
                                nn_cache[idx].key = 0; // Invalidate key
                            }
                            cache_locks[lock_idx].flag.clear(std::memory_order_release);
                        }
                        node->is_evaluating.store(false, std::memory_order_release);
                        revert_virtual_loss(path, path_size);
                        return;
                    }

                    if (result.first == nullptr) [[unlikely]] {
                        {
                            while (cache_locks[lock_idx].flag.test_and_set(std::memory_order_acquire)) {
                                _mm_pause();
                            }
                            if (nn_cache[idx].key == z_key && nn_cache[idx].policy == nullptr) {
                                nn_cache[idx].key = 0;
                            }
                            cache_locks[lock_idx].flag.clear(std::memory_order_release);
                        }
                        node->is_evaluating.store(false, std::memory_order_release);
                        revert_virtual_loss(path, path_size);
                        return;
                    }

                    {
                        std::shared_ptr<const std::vector<float>> old_policy;
                        while (cache_locks[lock_idx].flag.test_and_set(std::memory_order_acquire)) {
                            _mm_pause();
                        }
                        if (nn_cache[idx].key == z_key) {
                            old_policy = std::move(nn_cache[idx].policy);
                            nn_cache[idx].policy = result.first;
                            nn_cache[idx].value = result.second;
                        }
                        cache_locks[lock_idx].flag.clear(std::memory_order_release);
                    }

                    expand_node_with_moves(node, sim_board, l_moves, end_moves, result.first);
                    node->raw_value = result.second;
                    value = result.second;

                    node->is_expanded.store(true, std::memory_order_release);
                    node->is_evaluating.store(false, std::memory_order_release);
                    should_backprop = true;
                }
            }
        }
        else {
            value = node->raw_value;
            should_backprop = true;
        }

        if (should_backprop) {
            value = 1.0f - value;

            for (int i = path_size - 1; i >= 0; --i) {
                Node* n = path[i];
                if (i != 0) {
                    atomic_add_float(n->value_sum, get_virtual_loss(i) + value);
                }
                else {
                    n->visit_count.fetch_add(1, std::memory_order_relaxed);
                    atomic_add_float(n->value_sum, value);
                }
                value = 1.0f - value;
            }
        }
    }

    void expand_node_with_moves(Node* node, const Board& board, Move* moves_start, Move* moves_end, std::shared_ptr<const std::vector<float>> policy) {
        bool is_white = (board.turn != 0);
        size_t num_moves = moves_end - moves_start;
        if (num_moves == 0) return;

        // --- 1. まず Policy の合計値（policy_sum）を計算する ---
        float policy_sum = 0.0f;
        float valid_policies[MAX_LEGAL_MOVES];

        for (int i = 0; i < (int)num_moves; ++i) {
            Move check_m = *(moves_start + i);
            if (is_white) {
                if (!check_m.is_drop) check_m.from = 80 - check_m.from;
                check_m.to = 80 - check_m.to;
            }

            // ★ Vocabを使わずに直接インデックスを計算する
            int p_idx = get_action_index(check_m);

            // ★ 出力サイズは2187なので、その範囲に収まっているかチェック
            float p = (p_idx >= 0 && p_idx < 2187 && policy) ? (*policy)[p_idx] : 0.00001f;

            valid_policies[i] = p;
            policy_sum += p;
        }

        // ゼロ除算防止
        if (policy_sum < 0.000001f) policy_sum = 1.0f;

        // --- 2. 正規化（合計を1.0にする）しながらノードを確保・連結する ---
        Node* prev_child = nullptr;
        for (int i = 0; i < (int)num_moves; ++i) {
            Move m = *(moves_start + i);
            // ★ここが超重要：生の確率を合計値で割って、全体で100%(1.0)になるようにする
            float normalized_p = valid_policies[i] / policy_sum;

            // 正規化された正しい確率(normalized_p)でノードを作る
            Node* new_child = current_alloc().alloc(normalized_p, m);

            if (prev_child == nullptr) {
                node->first_child = new_child; // 最初の子
            }
            else {
                prev_child->sibling = new_child; // 兄弟として繋ぐ
            }
            prev_child = new_child;
        }
    }
};