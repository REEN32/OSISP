#include "platform.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

struct Config {
    int philosophers = 5, cycles = 100, think = 2, eat = 2, jitter = 1;
    uint32_t seed = 1000, timeout = 1000, duration = 60000;
    bool quiet = false, individual = false;
    std::string mode = "asymmetric", json;
};
int number(const std::string& s, int lo, int hi) {
    size_t end = 0; long long n = std::stoll(s, &end);
    if (end != s.size() || n < lo || n > hi) throw std::runtime_error("Invalid numeric value: " + s);
    return static_cast<int>(n);
}
Config parse(int argc, char** argv) {
    Config c;
    // Preset is applied first, explicit arguments always override it.
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--individual") c.individual = true;
    if (c.individual) { c.seed = 3619; c.cycles = 235; }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--individual") continue;
        if (a == "--quiet") { c.quiet = true; continue; }
        if (a == "--help") {
            std::cout << "Variant 4, IDs 0..N-1, left=i, right=(i+1)%N\n"
                      << "--individual (ID=027 seed=3619 cycles=235) --quiet\n"
                      << "--philosophers 2..32 --cycles 1..1000000 --think-ms 0..10000\n"
                      << "--eat-ms 0..10000 --jitter-ms 0..1000 --seed 0..2147483647\n"
                      << "--timeout-ms 1..60000 --duration-ms 1..3600000\n"
                      << "--mode asymmetric|ordered --json FILE\n";
            std::exit(0);
        }
        if (++i == argc) throw std::runtime_error("Missing value for " + a);
        const std::string v = argv[i];
        if (a == "--philosophers") c.philosophers = number(v, 2, 32);
        else if (a == "--cycles") c.cycles = number(v, 1, 1000000);
        else if (a == "--think-ms") c.think = number(v, 0, 10000);
        else if (a == "--eat-ms") c.eat = number(v, 0, 10000);
        else if (a == "--jitter-ms") c.jitter = number(v, 0, 1000);
        else if (a == "--seed") c.seed = static_cast<uint32_t>(number(v, 0, 2147483647));
        else if (a == "--timeout-ms") c.timeout = static_cast<uint32_t>(number(v, 1, 60000));
        else if (a == "--duration-ms") c.duration = static_cast<uint32_t>(number(v, 1, 3600000));
        else if (a == "--json") c.json = v;
        else if (a == "--mode" && (v == "asymmetric" || v == "ordered")) c.mode = v;
        else throw std::runtime_error("Unknown argument or value: " + a + " " + v);
    }
    return c;
}
struct Stats {
    uint64_t tid = 0;
    int completed = 0, attempts = 0, timeouts = 0;
    double wait_total = 0, wait_success = 0, wait_max = 0, eating_ms = 0;
};
struct Context;
struct Worker { Context* ctx; int id; Stats stats; };
struct Context {
    Config c;
    std::vector<std::unique_ptr<os::Mutex>> forks;
    std::vector<int> owners;
    std::vector<bool> eating;
    std::vector<Worker> workers;
    os::Event start, stop, done;
    os::Lock monitor, logger;
    std::atomic<int> violations{0}, errors{0}, exited{0};
    double started = 0;
    explicit Context(Config cfg): c(std::move(cfg)), owners(c.philosophers, -1), eating(c.philosophers, false) {
        for (int i = 0; i < c.philosophers; ++i) {
            forks.emplace_back(std::make_unique<os::Mutex>());
            workers.push_back({this, i, {}});
        }
    }
    void log(int id, const char* event, int resource = -1) {
        if (c.quiet) return;
        os::Guard g(logger);
        std::cout << '[' << std::fixed << std::setprecision(3) << os::now_ms() - started
                  << " ms] P" << id << " TID=" << os::tid() << ' ' << event;
        if (resource >= 0) std::cout << " RESOURCE=" << resource;
        std::cout << '\n';
    }
    void check(bool condition) { if (!condition) ++violations; }
};
// A guard is created before acquisition, so all acquired resources have an owner.
class ForkGuard {
    Context& x_; int id_, fork_; bool held_ = false;
public:
    ForkGuard(Context& x, int id, int fork): x_(x), id_(id), fork_(fork) {}
    bool acquire(uint32_t timeout) {
        x_.log(id_, "WAITING", fork_);
        if (!x_.forks[fork_]->acquire(x_.stop, timeout)) return false;
        held_ = true;
        { os::Guard g(x_.monitor); x_.check(x_.owners[fork_] == -1); x_.owners[fork_] = id_; }
        x_.log(id_, "ACQUIRED", fork_);
        return true;
    }
    ~ForkGuard() {
        if (!held_) return;
        try {
            // Monitor release precedes kernel release; new owner cannot enter yet.
            { os::Guard g(x_.monitor); x_.check(x_.owners[fork_] == id_); x_.owners[fork_] = -1; }
            x_.log(id_, "RELEASED", fork_);
            x_.forks[fork_]->release();
        } catch (...) { ++x_.errors; try { x_.stop.set(); } catch (...) {} }
    }
};
uint32_t random_next(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
uint32_t delay(int base, int jitter, uint32_t& s) {
    int delta = static_cast<int>(random_next(s) % static_cast<uint32_t>(2*jitter+1)) - jitter;
    return static_cast<uint32_t>(std::max(0, base+delta));
}
void run(Worker& w) {
    Context& x = *w.ctx; auto& s = w.stats; const int id = w.id, n = x.c.philosophers;
    s.tid = os::tid();
    while (!x.start.wait(50)) if (x.stop.wait(0)) return;
    uint32_t rng = x.c.seed ^ (0x9e3779b9u * static_cast<uint32_t>(id+1));
    if (!rng) rng = 1;
    for (int cycle = 0; cycle < x.c.cycles && !x.stop.wait(0); ++cycle) {
        x.log(id, "THINKING");
        if (x.stop.wait(delay(x.c.think, x.c.jitter, rng))) break;
        int left = id, right = (id+1)%n;
        int first = id%2 == 0 ? left : right;
        int second = id%2 == 0 ? right : left;
        if (x.c.mode == "ordered") { first = std::min(left, right); second = std::max(left, right); }
        const double waiting = os::now_ms();
        const double eating_before = s.eating_ms;
        bool ate = false;
        while (!x.stop.wait(0) && !ate) {
            ++s.attempts;
            bool retry = false;
            {
                ForkGuard a(x, id, first), b(x, id, second);
                const double attempt = os::now_ms();
                if (!a.acquire(x.c.timeout)) retry = true;
                else {
                    const double remaining = double(x.c.timeout) - (os::now_ms() - attempt);
                    if (remaining <= 0 || !b.acquire(static_cast<uint32_t>(std::ceil(remaining)))) retry = true;
                    else if (!x.stop.wait(0)) {
                        const double wait = os::now_ms() - waiting;
                        s.wait_max = std::max(s.wait_max, wait);
                        {
                            os::Guard g(x.monitor);
                            x.check(x.owners[left] == id && x.owners[right] == id);
                            x.check(!x.eating[id] && !x.eating[(id+n-1)%n] && !x.eating[(id+1)%n]);
                            x.eating[id] = true;
                            x.check(std::count(x.eating.begin(), x.eating.end(), true) <= n/2);
                        }
                        x.log(id, "EATING");
                        const double eating_start = os::now_ms();
                        bool interrupted = x.stop.wait(delay(x.c.eat, x.c.jitter, rng));
                        s.eating_ms += os::now_ms() - eating_start;
                        { os::Guard g(x.monitor); x.eating[id] = false; }
                        if (!interrupted) { ++s.completed; s.wait_success += wait; ate = true; }
                    }
                }
            } // reverse release, including first fork on timeout
            if (retry && !x.stop.wait(0)) {
                ++s.timeouts; x.log(id, "TIMEOUT");
                if (x.stop.wait(1 + random_next(rng)%3)) break;
            }
        }
        s.wait_total += os::now_ms() - waiting - (s.eating_ms - eating_before);
        if (ate) x.log(id, "CYCLE_DONE");
    }
    x.log(id, s.completed == x.c.cycles ? "FINISHED" : "CANCELLED");
}
THREAD_ENTRY entry(void* arg) {
    auto& w = *static_cast<Worker*>(arg); Context& x = *w.ctx;
    try { run(w); }
    catch (const std::exception& e) {
        ++x.errors;
        { os::Guard g(x.logger); std::cerr << "P" << w.id << " ERROR " << e.what() << '\n'; }
        try { x.stop.set(); } catch (...) {}
    }
    if (++x.exited == x.c.philosophers) { try { x.done.set(); } catch (...) { ++x.errors; } }
    return 0;
}
int execute(Config c) {
    Context x(std::move(c));
    std::vector<std::unique_ptr<os::Thread>> threads;
    threads.reserve(x.c.philosophers);
    x.started = os::now_ms();
    try {
        for (auto& w : x.workers) threads.emplace_back(std::make_unique<os::Thread>(entry, &w));
        x.start.set();
    } catch (...) {
        x.stop.set(); x.start.set();
        for (auto& t : threads) t->join();
        throw;
    }
    bool deadline = !x.done.wait(x.c.duration);
    if (deadline) x.stop.set();
    if (!x.done.wait(5000)) {
        std::cerr << "FATAL: workers did not stop within grace period\n";
        std::_Exit(3); // no destruction of objects still used by workers
    }
    for (auto& t : threads) t->join();
    const double elapsed = os::now_ms() - x.started;
    int completed = 0, timeouts = 0; double wait = 0, max_wait = 0, eat = 0;
    for (auto& w : x.workers) {
        completed += w.stats.completed; timeouts += w.stats.timeouts;
        wait += w.stats.wait_success; max_wait = std::max(max_wait, w.stats.wait_max); eat += w.stats.eating_ms;
    }
    for (int i = 0; i < x.c.philosophers; ++i) { x.check(x.owners[i] == -1); x.check(!x.eating[i]); }
    const bool success = !deadline && x.errors == 0 && x.violations == 0 && completed == x.c.philosophers*x.c.cycles;
    std::ostringstream out;
    out << std::fixed << std::setprecision(6)
        << "{\n  \"backend\":\"" << os::backend << "\",\n  \"mode\":\"" << x.c.mode
        << "\",\n  \"seed\":" << x.c.seed << ",\n  \"philosophers\":" << x.c.philosophers
        << ",\n  \"cycles\":" << x.c.cycles << ",\n  \"think_ms\":" << x.c.think
        << ",\n  \"eat_ms\":" << x.c.eat << ",\n  \"jitter_ms\":" << x.c.jitter
        << ",\n  \"timeout_ms\":" << x.c.timeout << ",\n  \"duration_ms\":" << x.c.duration
        << ",\n  \"elapsed_ms\":" << elapsed << ",\n  \"completed\":" << completed
        << ",\n  \"throughput_ops_s\":" << 1000.0*completed/elapsed
        << ",\n  \"mean_wait_ms\":" << (completed ? wait/completed : 0)
        << ",\n  \"max_wait_ms\":" << max_wait << ",\n  \"resource_utilization\":" << 2*eat/(x.c.philosophers*elapsed)
        << ",\n  \"timeouts\":" << timeouts << ",\n  \"violations\":" << x.violations.load()
        << ",\n  \"errors\":" << x.errors.load() << ",\n  \"deadline\":" << (deadline ? "true" : "false")
        << ",\n  \"success\":" << (success ? "true" : "false") << ",\n  \"workers\":[\n";
    for (size_t i = 0; i < x.workers.size(); ++i) {
        auto& s = x.workers[i].stats;
        out << "    {\"id\":" << i << ",\"tid\":" << s.tid << ",\"completed\":" << s.completed
            << ",\"attempts\":" << s.attempts << ",\"timeouts\":" << s.timeouts
            << ",\"wait_total_ms\":" << s.wait_total << ",\"wait_success_ms\":" << s.wait_success << ",\"max_wait_ms\":" << s.wait_max
            << ",\"eating_ms\":" << s.eating_ms << "}" << (i+1<x.workers.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
    std::cout << "BACKEND=" << os::backend << " COMPLETED=" << completed << '/' << x.c.philosophers*x.c.cycles
              << " ELAPSED_MS=" << elapsed << " TIMEOUTS=" << timeouts << " VIOLATIONS=" << x.violations
              << " ERRORS=" << x.errors << " STATUS=" << (success ? "OK" : "INCOMPLETE") << '\n';
    if (!x.c.json.empty()) {
        std::ofstream f(x.c.json); if (!f) throw std::runtime_error("Cannot create JSON: " + x.c.json);
        f << out.str(); f.close(); if (!f) throw std::runtime_error("Cannot write JSON");
    }
    return success ? 0 : 2;
}
int main(int argc, char** argv) {
    try { return execute(parse(argc, argv)); }
    catch (const std::exception& e) { std::cerr << "ERROR: " << e.what() << '\n'; return 1; }
}
