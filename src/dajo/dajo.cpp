// Dajo プロトコル DJP v0 の実装（設計書 §E9）。
// 命令は1行ずつ、メインの読み取りのスレッドで処理する。ジョブはドライバのスレッドで動き、結果は番号つきで返す。
#include "dajo/dajo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <sstream>

#include "eval/raypair/raypair.h"

namespace dajo {
namespace {

constexpr int kProtocolVersion = 0;   // 対応している版（§E9.2）
constexpr long long kMaxId = 2147483647LL;
constexpr double kHalfKpScale = 600.0;  // HalfKP の既定の eval_scale（重みファイルに値が無いため）

using Clock = std::chrono::steady_clock;

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string tok;
    while (in >> tok) out.push_back(tok);
    return out;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string cur;
    while (std::getline(in, cur, ',')) {
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) out += (i ? sep : "") + v[i];
    return out;
}

bool to_int(const std::string& s, long long& v) {
    try {
        size_t p = 0;
        v = std::stoll(s, &p);
        return p == s.size();
    }
    catch (...) {
        return false;
    }
}

bool to_double(const std::string& s, double& v) {
    try {
        size_t p = 0;
        v = std::stod(s, &p);
        return p == s.size();
    }
    catch (...) {
        return false;
    }
}

std::string fixed4(double x) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", x);
    return b;
}

// 勝率 wr = 1 / (1 + exp(−cp / eval_scale))（§E9.1 の 6）
double wr_of(long long cp, double scale) { return 1.0 / (1.0 + std::exp(-(double)cp / scale)); }

// 内部の評価値（詰みを含む）から、勝率。詰みなら勝ちは 1、負けは 0
double wr_for(int s, double scale) {
    if (s >= search::kMateBound) return 1.0;
    if (s <= -search::kMateBound) return 0.0;
    return wr_of(s, scale);
}

// 詰みの表記（§E9.7）：mate=+n（n 手で勝ち）／mate=-n（n 手で負け）。詰みでなければ空
std::string mate_field(int s) {
    if (s >= search::kMateBound) return " mate=+" + std::to_string(search::kMate - s);
    if (s <= -search::kMateBound) return " mate=-" + std::to_string(search::kMate + s);
    return "";
}

const char* bound_name(int b) {
    if (b == search::kExact) return "exact";
    return b == search::kLower ? "lower" : "upper";
}

std::string pv_text(const std::vector<Move>& pv) {
    std::string out;
    for (size_t i = 0; i < pv.size(); i++) out += (i ? "," : "") + move_to_usi(pv[i]);
    return out;
}

// 根の値の要約（result の行に付ける）
std::string summary(const search::RootMove& r, double scale) {
    return " score=" + std::to_string(r.score) + " wr=" + fixed4(wr_for(r.score, scale)) + mate_field(r.score) +
           " bound=" + bound_name(r.bound) + " depth=" + std::to_string(r.depth) +
           " seldepth=" + std::to_string(r.seldepth) + " pv=" + pv_text(r.pv);
}

// 評価関数の重みを、局面に付ける（局面を作る前に呼ぶ）
void attach(Game& g, const search::EvalRef& ev) {
    if (ev.hk) g.set_halfkp(ev.hk);
    else if (ev.net) g.set_network(ev.net);
}

enum class PosErr { kNone, kIllegalPosition, kIllegalMove };

// 局面（startpos または sfen の4語）と、その後の moves を t[i..end) から作る（§E9.3）
PosErr build_position(const std::vector<std::string>& t, size_t i, size_t end, Game& g) {
    if (i >= end) return PosErr::kIllegalPosition;
    if (t[i] == "startpos") {
        g.set_startpos();
        i++;
    }
    else if (t[i] == "sfen") {
        if (i + 4 >= end) return PosErr::kIllegalPosition;
        // 盤面は9段（区切りは8個）、手番は b か w。玉が両方そろっていなければ、局面として使わない
        if (std::count(t[i + 1].begin(), t[i + 1].end(), '/') != 8) return PosErr::kIllegalPosition;
        if (t[i + 2] != "b" && t[i + 2] != "w") return PosErr::kIllegalPosition;
        try {
            g.set_sfen(t[i + 1] + " " + t[i + 2] + " " + t[i + 3] + " " + t[i + 4]);
        }
        catch (...) {
            return PosErr::kIllegalPosition;
        }
        if (g.board.king_sq[0] < 0 || g.board.king_sq[1] < 0) return PosErr::kIllegalPosition;
        i += 5;
    }
    else {
        return PosErr::kIllegalPosition;
    }
    if (i < end && t[i] == "moves") i++;
    for (; i < end; i++) {
        if (!g.push_usi(t[i])) return PosErr::kIllegalMove;
    }
    return PosErr::kNone;
}

// set の値を検査し、commit のときに反映する。知らないキーは無視する。値が不正なら false
bool apply_one(search::Options& o, double& scale, bool& reconf, const std::string& k, const std::string& v, bool commit) {
    long long n = 0;
    double d = 0;
    if (k == "threads") {
        if (!to_int(v, n) || n < 1 || n > 256) return false;
        if (commit) { o.threads = (int)n; reconf = true; }
    }
    else if (k == "affinity") {
        if (commit) { o.cpu_affinity = v; reconf = true; }
    }
    else if (k == "physical_only") {
        if (!to_int(v, n) || (n != 0 && n != 1)) return false;
        if (commit) { o.physical_only = n == 1; reconf = true; }
    }
    else if (k == "hash") {
        if (!to_int(v, n) || n < 1 || n > 65536) return false;
        if (commit) { o.hash_mb = (int)n; reconf = true; }
    }
    else if (k == "draw_value") {
        if (!to_int(v, n) || n < -1000 || n > 1000) return false;
        if (commit) o.draw_value = (int)n;
    }
    else if (k == "max_ply") {
        if (!to_int(v, n) || n < 0 || n > 10000) return false;
        if (commit) o.max_ply = (int)n;
    }
    else if (k == "hint_weight") {
        if (!to_double(v, d) || d < 0 || d > 1) return false;
        if (commit) o.hint_weight = d;
    }
    else if (k == "hint_lmr_threshold") {
        if (!to_double(v, d) || d < 0 || d > 1) return false;
        if (commit) o.hint_lmr_threshold = d;
    }
    else if (k == "deterministic") {
        if (!to_int(v, n) || (n != 0 && n != 1)) return false;
        if (commit) o.deterministic = n == 1;
    }
    else if (k == "eval_scale") {
        if (!to_double(v, d) || d <= 0) return false;
        if (commit) scale = d;
    }
    return true;
}

}  // namespace

// ---------- ジョブ（1つの番号ぶん） ----------

struct Dajo::Entry {
    int id = 0;
    int serial = 0;                  // ドライバのスレッドの番号（drivers_ の鍵）
    int threads = 1;                 // 割り当てるスレッド数
    int priority = 5;
    bool report_iter = true;         // report=iter（反復ごとに出す）か none（最後だけ）
    int lines = 1;                   // 根の手の値を出す行数
    bool started = false;
    bool cancelled = false;
    search::Options opt;             // 受け付けたときの設定の複製
    search::EvalRef ev;              // 評価の重み（main の重みは、動いているあいだ変わらない）
    double scale = kHalfKpScale;     // wr の換算に使う eval_scale
    std::shared_ptr<search::HintTable> hints;  // 作った時点のヒントの表（以後、書き換えない）
    std::shared_ptr<search::Job> current;      // いま動いている部分のジョブ（cancel・promote の相手）
    std::vector<search::RootMove> last;        // report=none のときの、最後に読み切った段
    long long nodes = 0;
    Clock::time_point t0;
    std::function<Outcome(Entry&)> body;       // ドライバで動く本体
};

// ---------- 窓口 ----------

Dajo::Dajo(Host host) : host_(std::move(host)) {}

Dajo::~Dajo() {
    cancel_all();
    join_done();
}

void Dajo::out(const std::string& s) { host_.out(s); }

void Dajo::error(long long id, const std::string& code, const std::string& extra) {
    out("dajo error " + std::to_string(id) + " code=" + code + (extra.empty() ? "" : " " + extra));
}

void Dajo::handle(const std::string& line) {
    const std::vector<std::string> t = split(line);
    if (t.empty() || t[0] != "dajo") return;
    reap();
    const std::string cmd = t.size() > 1 ? t[1] : "";
    if (cmd == "hello") {
        hello(t);
        return;
    }
    if (!enabled_) return;  // hello の前は、ほかの命令を黙って捨てる（§E9.1）
    if (cmd == "set") set(t);
    else if (cmd == "sysinfo") out("dajo sysinfo " + host_.sysinfo());
    else if (cmd == "sync") out("dajo synced");
    else if (cmd == "newgame") newgame();
    else if (cmd == "cancel") cancel(t);
    else if (cmd == "promote") promote(t);
    else if (cmd == "hint") hint(t);
    else if (cmd == "job") job(t);
    else error(0, "unknown_command");
}

bool Dajo::busy() {
    std::lock_guard<std::mutex> lk(mu_);
    return !active_.empty();
}

void Dajo::cancel_all() {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& kv : active_) ids.push_back(kv.first);
    }
    for (int id : ids) cancel_id(id);
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return active_.empty(); });
    lk.unlock();
    join_done();
}

void Dajo::reap() {
    join_done();
    bool apply = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        capacity_ = std::max(1, host_.opt->threads);
        if (pending_reconfigure_ && active_.empty()) {
            pending_reconfigure_ = false;
            apply = true;
        }
    }
    if (apply) host_.reconfigure();
}

void Dajo::join_done() {
    std::vector<std::thread> ready;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int s : done_) {
            auto it = drivers_.find(s);
            if (it == drivers_.end()) continue;
            ready.push_back(std::move(it->second));
            drivers_.erase(it);
        }
        done_.clear();
    }
    for (std::thread& t : ready) {
        if (t.joinable()) t.join();
    }
}

bool Dajo::in_use(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    return active_.count(id) > 0;
}

// hello：版を返す（§E9.2）。対応するのは版 0 だけ
void Dajo::hello(const std::vector<std::string>& t) {
    long long v = 0;
    if (t.size() < 3 || !to_int(t[2], v)) {
        error(0, "bad_arg", "key=version");
        return;
    }
    if (v < 0) {
        error(0, "version");
        return;
    }
    enabled_ = true;
    out("dajo hello " + std::to_string(kProtocolVersion) +
        " engine=Nuenagon caps=search,rank,probe,threat,mate,eval,hint,promote,sysinfo");
}

// set：設定を変える（§E9.5）。プールと置換表に関わるものは、ジョブが無いときに反映する
void Dajo::set(const std::vector<std::string>& t) {
    std::vector<std::pair<std::string, std::string>> kvs;
    for (size_t i = 2; i < t.size(); i++) {
        const size_t eq = t[i].find('=');
        if (eq == std::string::npos) continue;
        kvs.emplace_back(t[i].substr(0, eq), t[i].substr(eq + 1));
    }
    double scale = eval_scale_;
    bool reconf = false;
    for (const auto& kv : kvs) {
        bool dummy = false;
        if (!apply_one(*host_.opt, scale, dummy, kv.first, kv.second, false)) {
            error(0, "bad_arg", "key=" + kv.first);
            return;
        }
    }
    for (const auto& kv : kvs) apply_one(*host_.opt, scale, reconf, kv.first, kv.second, true);
    {
        std::lock_guard<std::mutex> lk(mu_);
        eval_scale_ = scale;
        capacity_ = std::max(1, host_.opt->threads);
    }
    if (reconf) {
        if (busy()) {
            std::lock_guard<std::mutex> lk(mu_);
            pending_reconfigure_ = true;
        }
        else {
            host_.reconfigure();
        }
    }
    out("dajo set ok");
}

// newgame：置換表を消し、ヒントを消す（§E9.5）。決定的モードで同じ結果を返すために、置換表は空にする
void Dajo::newgame() {
    host_.tt->clear();
    *host_.hints = std::make_shared<search::HintTable>();
}

void Dajo::cancel(const std::vector<std::string>& t) {
    if (t.size() < 3) {
        error(0, "bad_arg", "key=id");
        return;
    }
    if (t[2] == "all") {
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& kv : active_) ids.push_back(kv.first);
        }
        for (int id : ids) cancel_id(id);
        return;
    }
    long long id = 0;
    if (!to_int(t[2], id) || id < 1 || id > kMaxId) {
        error(0, "bad_arg", "key=id");
        return;
    }
    if (!cancel_id((int)id)) error(id, "no_such_job");
}

// 止める。待っているジョブは、動かさずに result を返す。動いているジョブは、止めて result はドライバが返す
bool Dajo::cancel_id(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = active_.find(id);
    if (it == active_.end()) return false;
    std::shared_ptr<Entry> e = it->second;
    if (!e->started) {
        queue_.erase(std::remove(queue_.begin(), queue_.end(), e), queue_.end());
        active_.erase(it);
        out("dajo result " + std::to_string(id) + " status=cancelled time=0 nodes=0");
        cv_.notify_all();
        return true;
    }
    e->cancelled = true;
    if (e->current) e->current->stop();
    return true;
}

// promote：動いているジョブに上限を付ける（§E9.5）。DJP での ponder に使う
void Dajo::promote(const std::vector<std::string>& t) {
    long long id = 0;
    if (t.size() < 3 || !to_int(t[2], id) || id < 1 || id > kMaxId) {
        error(0, "bad_arg", "key=id");
        return;
    }
    long long time = 0, nodes = 0, depth = 0;
    for (size_t i = 3; i < t.size(); i++) {
        const size_t eq = t[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = t[i].substr(0, eq);
        long long* dst = k == "time" ? &time : k == "nodes" ? &nodes : k == "depth" ? &depth : nullptr;
        if (!dst || !to_int(t[i].substr(eq + 1), *dst) || *dst < 0) {
            error((int)id, "bad_arg", "key=" + k);
            return;
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    auto it = active_.find((int)id);
    if (it == active_.end()) {
        error(id, "no_such_job");
        return;
    }
    if (!it->second->started || !it->second->current) {
        error(id, "busy");
        return;
    }
    it->second->current->extend(time, nodes, (int)depth);
    out("dajo promoted " + std::to_string(id));
}

// hint：局面のヒント（§E5.5）を登録する。clear なら全部消す
void Dajo::hint(const std::vector<std::string>& t) {
    if (t.size() >= 3 && t[2] == "clear") {
        *host_.hints = std::make_shared<search::HintTable>();
        return;
    }
    std::string probs;
    size_t pos = 2;
    while (pos < t.size() && t[pos] != "position") {
        if (t[pos].rfind("probs=", 0) == 0) probs = t[pos].substr(6);
        pos++;
    }
    Game g;
    attach(g, host_.eval());
    if (build_position(t, pos + 1, t.size(), g) != PosErr::kNone) {
        error(0, "illegal_position");
        return;
    }
    const std::vector<Move> legal = g.legal_moves();
    std::vector<search::HintEntry> entries;
    for (const std::string& item : split_csv(probs)) {
        const size_t c = item.find(':');
        double p = 0;
        if (c == std::string::npos || !to_double(item.substr(c + 1), p) || p < 0 || p > 1) {
            error(0, "bad_arg", "key=probs");
            return;
        }
        const std::string u = item.substr(0, c);
        auto it = std::find_if(legal.begin(), legal.end(), [&u](const Move& m) { return move_to_usi(m) == u; });
        if (it == legal.end()) {
            error(0, "illegal_move");
            return;
        }
        search::HintEntry h;
        h.move = *it;
        h.prob = p;
        entries.push_back(h);
    }
    // 動いているジョブが見ている表は書き換えず、書き換える前に複製する
    std::shared_ptr<search::HintTable>& hp = *host_.hints;
    if (hp.use_count() > 1) hp = std::make_shared<search::HintTable>(*hp);
    hp->set(g.board.zobrist_key, std::move(entries));
}

// job：ジョブを受け付ける（§E9.4）。eval は動かさずに、すぐ結果を返す
void Dajo::job(const std::vector<std::string>& t) {
    long long idv = 0;
    if (t.size() < 4 || !to_int(t[2], idv) || idv < 1 || idv > kMaxId) {
        error(0, "bad_arg", "key=id");
        return;
    }
    const int id = (int)idv;
    const std::string kind = t[3];
    const bool is_eval = kind == "eval";
    if (!(kind == "search" || kind == "rank" || kind == "probe" || kind == "threat" || kind == "mate" || is_eval)) {
        error(id, "bad_arg", "key=kind");
        return;
    }
    if (in_use(id)) {
        error(id, "duplicate_id");
        return;
    }
    size_t pos = 4;
    while (pos < t.size() && t[pos] != "position" && t[pos] != "positions") pos++;
    if (pos >= t.size() || (t[pos] == "positions") != is_eval) {
        error(id, "bad_arg", "key=position");
        return;
    }
    std::map<std::string, std::string> kv;
    for (size_t i = 4; i < pos; i++) {
        const size_t eq = t[i].find('=');
        if (eq == std::string::npos) {
            error(id, "bad_arg", "key=" + t[i]);
            return;
        }
        kv[t[i].substr(0, eq)] = t[i].substr(eq + 1);
    }

    // 数の項目を読む。無ければ def、範囲外や不正なら false
    auto num = [&kv](const char* key, long long def, long long lo, long long hi, long long& v) {
        auto it = kv.find(key);
        if (it == kv.end()) {
            v = def;
            return true;
        }
        return to_int(it->second, v) && v >= lo && v <= hi;
    };
    auto bad = [this, id](const char* key) { error(id, "bad_arg", std::string("key=") + key); };

    search::Limits lim;
    long long time = 0, nodes = 0, depth = 0, inf = 0, threads = 0, priority = 0, multipv = 0;
    if (!num("time", 0, 0, 1000000000LL, time)) return bad("time");
    if (!num("nodes", 0, 0, 1000000000000LL, nodes)) return bad("nodes");
    if (!num("depth", 0, 0, 60, depth)) return bad("depth");
    if (!num("infinite", 0, 0, 1, inf)) return bad("infinite");
    if (!num("threads", 0, 0, 256, threads)) return bad("threads");
    if (!num("priority", 5, 0, 9, priority)) return bad("priority");
    if (!num("multipv", 1, 1, 500, multipv)) return bad("multipv");
    std::string report = kv.count("report") ? kv["report"] : "iter";
    if (report != "iter" && report != "none") return bad("report");

    const search::EvalRef ref = host_.eval();
    if (!ref.net && !ref.hk) {
        error(id, "bad_arg", "msg=no_eval");
        return;
    }
    const double scale = eval_scale_ > 0 ? eval_scale_ : (ref.hk ? kHalfKpScale : ref.scale);

    // 局面（eval は局面ごとに）
    Game g;
    attach(g, ref);
    if (is_eval) {
        std::vector<std::pair<size_t, size_t>> seg;  // 局面ごとの [始め, 終わり)
        size_t s = pos + 1;
        for (size_t i = pos + 1; i <= t.size(); i++) {
            if (i == t.size() || t[i] == "|") {
                seg.emplace_back(s, i);
                s = i + 1;
            }
        }
        std::vector<Game> games;
        for (const auto& se : seg) {
            Game gi;
            attach(gi, ref);
            const PosErr pe = build_position(t, se.first, se.second, gi);
            if (pe == PosErr::kIllegalMove) {
                error(id, "illegal_move");
                return;
            }
            if (pe != PosErr::kNone) {
                error(id, "illegal_position");
                return;
            }
            games.push_back(gi);
        }
        const bool qsearch = kv.count("qsearch") && kv["qsearch"] == "1";
        const Clock::time_point t0 = Clock::now();
        out("dajo accepted " + std::to_string(id));
        out("dajo started " + std::to_string(id));
        for (size_t i = 0; i < games.size(); i++) {
            long long cp = 0;
            if (qsearch) {
                cp = search::qsearch_score(games[i], ref);
            }
            else {
                const int32_t raw = games[i].raw_eval();
                cp = ref.hk ? (long long)(raw / ref.fv_scale) : raypair::cp_of(raw, ref.scale);
            }
            out("dajo evalres " + std::to_string(id) + " index=" + std::to_string(i) + " cp=" + std::to_string(cp) +
                " wr=" + fixed4(wr_of(cp, scale)));
        }
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        out("dajo result " + std::to_string(id) + " status=done time=" + std::to_string(ms) + " nodes=0");
        return;
    }

    if (time == 0 && nodes == 0 && depth == 0 && inf == 0) {
        error(id, "no_limit");
        return;
    }
    lim.movetime = time;
    lim.nodes = nodes;
    lim.depth = (int)depth;
    lim.infinite = inf == 1;

    if (t[pos] == "position") {
        const PosErr pe = build_position(t, pos + 1, t.size(), g);
        if (pe == PosErr::kIllegalMove) {
            error(id, "illegal_move");
            return;
        }
        if (pe != PosErr::kNone) {
            error(id, "illegal_position");
            return;
        }
    }
    const bool in_check = g.board.in_check(g.board.turn);

    // 種類ごとの項目と、本体
    search::Options opt = *host_.opt;
    std::function<Outcome(Entry&)> body;
    int lines = 1;
    if (kind == "search") {
        opt.multipv = (int)multipv;
        lines = (int)multipv;
        lim.searchmoves = split_csv(kv["searchmoves"]);
        lim.excludemoves = split_csv(kv["exclude"]);
        body = [this, g, lim](Entry& e) {
            auto j = make_job(e, g, lim, e.opt, false);
            j->set_root_report(move_reporter(e, e.lines));
            run_part(e, j);
            if (!e.report_iter && !e.last.empty()) print_moves(e, e.last);
            Outcome o;
            o.extra = "bestmove=" + j->best_move();
            if (!j->result().empty()) o.extra += summary(j->result()[0], e.scale);
            return o;
        };
    }
    else if (kind == "rank") {
        std::vector<std::string> moves = split_csv(kv["moves"]);
        const std::vector<std::string> legal = g.legal_moves_usi();
        if (moves.empty()) moves = legal;
        for (const std::string& u : moves) {
            if (std::find(legal.begin(), legal.end(), u) == legal.end()) {
                error(id, "illegal_move");
                return;
            }
        }
        opt.multipv = (int)moves.size();
        lines = (int)moves.size();
        lim.searchmoves = moves;
        lim.root_mate_probe = false;
        lim.exact_all = true;
        body = [this, g, lim](Entry& e) {
            auto j = make_job(e, g, lim, e.opt, false);
            j->set_root_report(move_reporter(e, e.lines));
            run_part(e, j);
            if (!e.report_iter && !e.last.empty()) print_moves(e, e.last);
            return Outcome{};
        };
    }
    else if (kind == "probe") {
        const std::string mv = kv["move"];
        const std::vector<std::string> legal = g.legal_moves_usi();
        if (mv.empty()) return bad("move");
        if (std::find(legal.begin(), legal.end(), mv) == legal.end()) {
            error(id, "illegal_move");
            return;
        }
        long long threshold = 0, mindepth = 0, ref_b = 0, margin = 0;
        if (!num("threshold", 0, -30000, 30000, threshold)) return bad("threshold");
        if (!num("mindepth", 4, 0, 60, mindepth)) return bad("mindepth");
        const bool has_margin = kv.count("margin") > 0;
        const bool has_ref = kv.count("ref") > 0;
        if (has_margin && !num("margin", 0, 0, 30000, margin)) return bad("margin");
        if (has_ref && !num("ref", 0, -30000, 30000, ref_b)) return bad("ref");
        lim.root_mate_probe = false;
        lim.searchmoves = {mv};
        body = [this, g, lim, mv, has_margin, has_ref, margin, ref_b, threshold, mindepth](Entry& e) {
            long long t_cp = threshold;
            if (has_margin) {
                // B（最善の評価）を、短い探索で求める。ref があれば、それを使う（§E9.4）
                long long b = ref_b;
                if (!has_ref) {
                    search::Limits lb;
                    lb.depth = 6;
                    lb.nodes = lim.nodes / 4;
                    lb.root_mate_probe = false;
                    search::Options ob = e.opt;
                    ob.threads = 1;
                    ob.multipv = 1;
                    auto jb = make_job(e, g, lb, ob, false);
                    b = run_part(e, jb) && !jb->result().empty() ? jb->result()[0].score : 0;
                }
                b = std::max<long long>(-search::kMateCp, std::min<long long>(search::kMateCp, b));
                t_cp = b - margin;
            }
            search::Limits lp = lim;
            lp.mode = search::JobMode::kProbe;
            lp.probe_threshold = (int)t_cp;
            lp.probe_mindepth = (int)mindepth;
            auto jp = make_job(e, g, lp, e.opt, false);
            run_part(e, jp);
            const search::ProbeOut& p = jp->probe();
            const char* verdict = p.verdict == 1 ? "yes" : p.verdict == 0 ? "no" : "unknown";
            Outcome o;
            o.extra = std::string("verdict=") + verdict + " threshold=" + std::to_string(t_cp);
            if (p.depth > 0) o.extra += " score=" + std::to_string(p.score) + " depth=" + std::to_string(p.depth);
            if (p.verdict >= 0) o.extra += std::string(" bound=") + (p.verdict == 1 ? "lower" : "upper");
            if (p.verdict == 0) o.extra += " pv=" + pv_text(p.pv);
            return o;
        };
    }
    else if (kind == "threat") {
        if (in_check) {
            error(id, "in_check");
            return;
        }
        const bool compare = kv.count("compare") && kv["compare"] == "1";
        const bool tsumero = kv.count("tsumero") && kv["tsumero"] == "1";
        lim.root_mate_probe = false;
        body = [this, g, lim, compare, tsumero](Entry& e) {
            Outcome o;
            // パスした局面での相手の最善手と、その評価（相手の手番の値なので、符号を変える）
            auto jp = make_job(e, g, lim, e.opt, true);
            run_part(e, jp);
            if (jp->result().empty()) {
                o.extra = "threat_move=none";
            }
            else {
                const search::RootMove& r = jp->result()[0];
                const int score_pass = -r.score;
                o.extra = "threat_move=" + move_to_usi(r.move) + " score_pass=" + std::to_string(score_pass);
                if (compare) {
                    auto jn = make_job(e, g, lim, e.opt, false);
                    run_part(e, jn);
                    if (!jn->result().empty()) {
                        const int score_normal = jn->result()[0].score;
                        o.extra += " score_normal=" + std::to_string(score_normal) +
                                   " gain=" + std::to_string(score_normal - score_pass);
                    }
                }
            }
            if (tsumero) {
                search::Limits lm = lim;
                lm.mode = search::JobMode::kMate;
                auto jm = make_job(e, g, lm, e.opt, true);
                run_part(e, jm);
                if (jm->mate_found()) o.extra += " tsumero=yes mate_pv=" + join(jm->mate_pv(), ",");
                else o.extra += " tsumero=unknown";
            }
            return o;
        };
    }
    else {  // mate
        const std::string side = kv.count("side") ? kv["side"] : "self";
        if (side != "self" && side != "opponent") return bad("side");
        const bool opponent = side == "opponent";
        if (opponent && in_check) {
            error(id, "in_check");
            return;
        }
        lim.root_mate_probe = false;
        body = [this, g, lim, opponent](Entry& e) {
            search::Limits lm = lim;
            lm.mode = search::JobMode::kMate;
            auto jm = make_job(e, g, lm, e.opt, opponent);
            run_part(e, jm);
            Outcome o;
            if (jm->mate_found()) {
                o.extra = "mate=yes length=" + std::to_string(jm->mate_pv().size()) + " pv=" + join(jm->mate_pv(), ",");
            }
            else {
                o.extra = "mate=unknown";
            }
            return o;
        };
    }

    // 受け付ける：スレッド数（決定的モードでは 1）と優先度
    const int capacity = std::max(1, host_.opt->threads);
    const int want = threads > 0 ? (int)std::min<long long>(threads, capacity) : capacity;
    opt.threads = opt.deterministic ? 1 : want;
    opt.multipv = opt.multipv < 1 ? 1 : opt.multipv;

    auto e = std::make_shared<Entry>();
    e->id = id;
    e->threads = opt.threads;
    e->priority = (int)priority;
    e->report_iter = report == "iter";
    e->lines = lines;
    e->opt = opt;
    e->ev = ref;
    e->scale = scale;
    e->hints = *host_.hints;
    e->t0 = Clock::now();
    e->body = std::move(body);
    admit(e);
}

// ---------- 割り当て ----------

// 受け付けて、空きがあれば動かす。足りなければ待たせる（優先度の高い順）
void Dajo::admit(const std::shared_ptr<Entry>& e) {
    std::lock_guard<std::mutex> lk(mu_);
    active_[e->id] = e;
    out("dajo accepted " + std::to_string(e->id));
    if (queue_.empty() && used_ + e->threads <= capacity_) {
        start_locked(e);
        return;
    }
    out("dajo queued " + std::to_string(e->id));
    auto at = std::find_if(queue_.begin(), queue_.end(),
                           [&e](const std::shared_ptr<Entry>& q) { return q->priority < e->priority; });
    queue_.insert(at, e);
}

// 動かし始める（mu_ を持っている）。ドライバのスレッドを作る
void Dajo::start_locked(const std::shared_ptr<Entry>& e) {
    e->started = true;
    e->t0 = Clock::now();
    used_ += e->threads;
    e->serial = ++serial_;
    out("dajo started " + std::to_string(e->id));
    drivers_.emplace(e->serial, std::thread([this, e] { run_driver(e); }));
}

// 空きがあるかぎり、待っているジョブを優先度の順に動かす。先頭が入らなければ止める
void Dajo::pump_locked() {
    while (!queue_.empty() && used_ + queue_.front()->threads <= capacity_) {
        std::shared_ptr<Entry> e = queue_.front();
        queue_.erase(queue_.begin());
        start_locked(e);
    }
}

void Dajo::run_driver(const std::shared_ptr<Entry>& e) {
    Outcome o;
    try {
        o = e->body(*e);
    }
    catch (const std::exception& ex) {
        o.status = "error";
        o.extra = std::string("msg=") + ex.what();
    }
    finish(*e, o);
}

// 結果を返して、割り当てを返す（ドライバのスレッドから呼ぶ）
void Dajo::finish(Entry& e, const Outcome& o) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::string status = e.cancelled ? "cancelled" : o.status;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - e.t0).count();
    out("dajo result " + std::to_string(e.id) + " status=" + status + " time=" + std::to_string(ms) +
        " nodes=" + std::to_string(e.nodes) + (o.extra.empty() ? "" : " " + o.extra));
    used_ -= e.threads;
    active_.erase(e.id);
    done_.push_back(e.serial);
    pump_locked();
    cv_.notify_all();
}

// 部分のジョブを動かす。すでに止められていれば動かさずに false を返す
bool Dajo::run_part(Entry& e, const std::shared_ptr<search::Job>& j) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (e.cancelled) return false;
        e.current = j;
    }
    j->run();
    std::lock_guard<std::mutex> lk(mu_);
    e.nodes += j->nodes();
    e.current.reset();
    return true;
}

std::shared_ptr<search::Job> Dajo::make_job(Entry& e, const Game& g, const search::Limits& lim,
                                            const search::Options& opt, bool null_root) {
    return std::make_shared<search::Job>(g, lim, opt, e.ev, *host_.tt, host_.pool(), e.hints.get(), null_root, nullptr);
}

// 根の手の値を、反復ごとに move の行として出す（report=none なら最後に出すため、保存する）
search::RootReport Dajo::move_reporter(Entry& e, int lines) {
    return [this, &e, lines](int, const std::vector<search::RootMove>& rm, long long, long long) {
        const size_t n = std::min<size_t>((size_t)lines, rm.size());
        std::vector<search::RootMove> part(rm.begin(), rm.begin() + n);
        if (!e.report_iter) {
            e.last = part;
            return;
        }
        print_moves(e, part);
    };
}

// move の行（§E9.7）
void Dajo::print_moves(Entry& e, const std::vector<search::RootMove>& rm) {
    for (const search::RootMove& r : rm) {
        out("dajo move " + std::to_string(e.id) + " move=" + move_to_usi(r.move) + " score=" + std::to_string(r.score) +
            " wr=" + fixed4(wr_for(r.score, e.scale)) + mate_field(r.score) + " bound=" + bound_name(r.bound) +
            " depth=" + std::to_string(r.depth) + " seldepth=" + std::to_string(r.seldepth) +
            " nodes=" + std::to_string(r.nodes) + " pv=" + pv_text(r.pv));
    }
}

}  // namespace dajo
