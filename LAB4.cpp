// Лабораторная работа 4. Вариант 4. STUDENT_ID = 027.
// Windows, Visual Studio, C++17. Один исходный файл.
#define NOMINMAX
#include <windows.h>
#include <process.h>
#include <cerrno>
#include <filesystem>
#include <utility>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace os {
inline std::runtime_error error(const char* name) {
    return std::runtime_error(std::string(name) + " GetLastError=" + std::to_string(GetLastError()));
}
class Handle {
    HANDLE h_;
public:
    explicit Handle(HANDLE h): h_(h) { if (!h_) throw error("Create object"); }
    ~Handle() { if (!CloseHandle(h_)) std::terminate(); }
    Handle(const Handle&) = delete;
    HANDLE get() const { return h_; }
};
class Event {
    Handle h_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
public:
    void set() { if (!SetEvent(h_.get())) throw error("SetEvent"); }
    bool wait(uint32_t ms) {
        DWORD r = WaitForSingleObject(h_.get(), ms);
        if (r == WAIT_OBJECT_0) return true;
        if (r == WAIT_TIMEOUT) return false;
        throw error("WaitForSingleObject event");
    }
    HANDLE get() const { return h_.get(); }
};
class Mutex {
    Handle h_{CreateMutexW(nullptr, FALSE, nullptr)};
public:
    bool acquire(Event& stop, uint32_t ms) {
        HANDLE hs[] = {stop.get(), h_.get()};
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, ms);
        if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT) return false;
        if (r == WAIT_OBJECT_0 + 1) return true;
        if (r == WAIT_ABANDONED_0 + 1) {
            if (!ReleaseMutex(h_.get())) throw error("Release abandoned Mutex");
            throw std::runtime_error("Abandoned Mutex: shared state is unreliable");
        }
        throw error("WaitForMultipleObjects");
    }
    void release() { if (!ReleaseMutex(h_.get())) throw error("ReleaseMutex"); }
};
class Lock {
    CRITICAL_SECTION cs_;
public:
    Lock() { if (!InitializeCriticalSectionEx(&cs_, 0, 0)) throw error("InitializeCriticalSectionEx"); }
    ~Lock() { DeleteCriticalSection(&cs_); }
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
};
using Entry = unsigned (__stdcall *)(void*);
class Thread {
    Handle h_;
public:
    static HANDLE create(Entry entry, void* arg) {
        const uintptr_t h = _beginthreadex(nullptr, 0, entry, arg, 0, nullptr);
        if (!h) throw std::runtime_error("_beginthreadex errno=" + std::to_string(errno));
        return reinterpret_cast<HANDLE>(h);
    }
    Thread(Entry entry, void* arg): h_(create(entry, arg)) {}
    void join() {
        DWORD r = WaitForSingleObject(h_.get(), 5000);
        if (r != WAIT_OBJECT_0) throw std::runtime_error("Thread join failed or exceeded 5 s");
    }
};
inline uint64_t tid() { return GetCurrentThreadId(); }
inline double now_ms() {
    static const double f = [] { LARGE_INTEGER x; if (!QueryPerformanceFrequency(&x)) throw error("QPF"); return double(x.QuadPart); }();
    LARGE_INTEGER x; if (!QueryPerformanceCounter(&x)) throw error("QPC");
    return double(x.QuadPart) * 1000.0 / f;
}
#define THREAD_ENTRY unsigned __stdcall
class Guard {
    Lock& l_;
public:
    explicit Guard(Lock& l): l_(l) { l_.lock(); }
    ~Guard() { l_.unlock(); }
    Guard(const Guard&) = delete;
};
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

struct Config {
    int philosophers = 5, cycles = 100, think = 2, eat = 2;
    uint32_t seed = 1000, timeout = 1000, duration = 60000;
    bool quiet = false;
};
int number(const std::string& s, int lo, int hi) {
    size_t end = 0; long long n = std::stoll(s, &end);
    if (end != s.size() || n < lo || n > hi) throw std::runtime_error("Invalid numeric value: " + s);
    return static_cast<int>(n);
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
    volatile LONG violations = 0, errors = 0, exited = 0;
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
    void check(bool condition) { if (!condition) InterlockedIncrement(&violations); }
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
            x_.forks[fork_]->release();
            x_.log(id_, "RELEASED", fork_);
        } catch (...) { InterlockedIncrement(&x_.errors); try { x_.stop.set(); } catch (...) {} }
    }
};
uint32_t random_next(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
void run(Worker& w) {
    Context& x = *w.ctx; auto& s = w.stats; const int id = w.id, n = x.c.philosophers;
    s.tid = os::tid();
    while (!x.start.wait(50)) if (x.stop.wait(0)) return;
    uint32_t rng = x.c.seed ^ (0x9e3779b9u * static_cast<uint32_t>(id+1));
    if (!rng) rng = 1;
    for (int cycle = 0; cycle < x.c.cycles && !x.stop.wait(0); ++cycle) {
        x.log(id, "THINKING");
        if (x.stop.wait(static_cast<uint32_t>(x.c.think))) break;
        int left = id, right = (id+1)%n;
        int first = id%2 == 0 ? left : right;
        int second = id%2 == 0 ? right : left;
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
                        bool interrupted = x.stop.wait(1 + random_next(rng) % static_cast<uint32_t>(x.c.eat));
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
        InterlockedIncrement(&x.errors);
        { os::Guard g(x.logger); std::cerr << "P" << w.id << " ERROR " << e.what() << '\n'; }
        try { x.stop.set(); } catch (...) {}
    }
    if (InterlockedIncrement(&x.exited) == x.c.philosophers) { try { x.done.set(); } catch (...) { InterlockedIncrement(&x.errors); } }
    return 0;
}
struct Measurement {
    double elapsed = 0, throughput = 0, mean_wait = 0, max_wait = 0;
    int completed = 0, timeouts = 0, violations = 0;
};

int execute(Config c, Measurement* measurement = nullptr) {
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
    int completed = 0, timeouts = 0; double wait = 0, max_wait = 0;
    for (auto& w : x.workers) {
        completed += w.stats.completed; timeouts += w.stats.timeouts;
        wait += w.stats.wait_success; max_wait = std::max(max_wait, w.stats.wait_max);
    }
    for (int i = 0; i < x.c.philosophers; ++i) { x.check(x.owners[i] == -1); x.check(!x.eating[i]); }
    const bool success = !deadline && x.errors == 0 && x.violations == 0 && completed == x.c.philosophers*x.c.cycles;
    std::cout << std::fixed << std::setprecision(3)
              << "COMPLETED=" << completed << '/' << x.c.philosophers*x.c.cycles
              << " ELAPSED_MS=" << elapsed << " TIMEOUTS=" << timeouts
              << " VIOLATIONS=" << x.violations << " ERRORS=" << x.errors
              << " STATUS=" << (success ? "OK" : "INCOMPLETE") << '\n';
    std::cout << "ID TID COMPLETED TIMEOUTS MEAN_WAIT_MS MAX_WAIT_MS\n";
    for (const auto& w : x.workers) {
        const auto& s = w.stats;
        std::cout << w.id << ' ' << s.tid << ' ' << s.completed << ' '
                  << s.timeouts << ' ' << (s.completed ? s.wait_success/s.completed : 0)
                  << ' ' << s.wait_max << '\n';
    }
    if (measurement) *measurement = {elapsed, 1000.0*completed/elapsed,
        completed ? wait/completed : 0, max_wait, completed, timeouts, x.violations};
    return success ? 0 : 2;
}

int input_number(const char* prompt, int fallback, int lo, int hi) {
    for (;;) {
        std::cout << prompt << " [" << fallback << "]: ";
        std::string s;
        if (!std::getline(std::cin, s)) throw std::runtime_error("Input closed");
        if (s.empty()) return fallback;
        try { return number(s, lo, hi); }
        catch (const std::exception&) { std::cout << "Enter a number from " << lo << " to " << hi << ".\n"; }
    }
}
struct ExperimentRow {
    std::string preset;
    int n, think, repeat;
    Measurement m;
};
void chart(const std::filesystem::path& path, const std::vector<ExperimentRow>& rows, bool waiting) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot create graph");
    double top = 0;
    for (const auto& row : rows) top = std::max(top, waiting ? row.m.mean_wait : row.m.throughput);
    top = std::max(1.0, top * 1.15);
    f << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1100\" height=\"520\" viewBox=\"0 0 1100 520\">"
      << "<rect width=\"1100\" height=\"520\" fill=\"white\"/><g font-family=\"Arial\" fill=\"black\">"
      << "<text x=\"550\" y=\"25\" text-anchor=\"middle\" font-size=\"18\">"
      << (waiting ? "Mean waiting for two forks (ms)" : "Throughput (completed cycles per second)")
      << " | Windows Mutex</text>";
    for (int panel = 0; panel < 2; ++panel) {
        const std::string preset = panel == 0 ? "standard" : "individual";
        const double x0 = 80 + 550*panel, y0 = 365, height = 270;
        f << "<text x=\"" << x0+180 << "\" y=\"65\" text-anchor=\"middle\" font-size=\"16\">"
          << preset << (panel ? " (235 cycles)" : " (100 cycles)") << "</text>";
        for (int tick = 0; tick <= 5; ++tick) {
            const double y = y0-height*tick/5;
            f << "<path d=\"M " << x0 << ' ' << y << " h 370\" stroke=\"#dddddd\"/>"
              << "<text x=\"" << x0-8 << "\" y=\"" << y+4 << "\" text-anchor=\"end\" font-size=\"11\">"
              << std::fixed << std::setprecision(2) << top*tick/5 << "</text>";
        }
        f << "<path d=\"M " << x0 << " 95 V 365 H " << x0+370 << "\" fill=\"none\" stroke=\"black\"/>";
        for (int n : {5, 7}) {
            const double x = x0+40+(n-5)*145;
            f << "<text x=\"" << x << "\" y=\"385\" text-anchor=\"middle\">" << n << "</text>";
        }
        f << "<text x=\"" << x0+185 << "\" y=\"410\" text-anchor=\"middle\">Number of philosophers</text>";
        int legend = 0;
        for (int think : {2,15}) {
            const char* color = think == 2 ? "#2166ac" : "#b35806";
            const char* dash = "";
            std::vector<std::pair<double,double>> points;
            for (int n : {5,7}) {
                double sum = 0; int count = 0;
                for (const auto& row : rows) if (row.preset == preset && row.n == n && row.think == think) {
                    sum += waiting ? row.m.mean_wait : row.m.throughput; ++count;
                }
                if (count != 3) throw std::runtime_error("Graph needs three real repetitions");
                points.push_back({x0+40+(n-5)*145, y0-height*(sum/count)/top});
            }
            f << "<polyline fill=\"none\" stroke=\"" << color << "\" stroke-width=\"2\"" << dash << " points=\"";
            for (auto point : points) f << point.first << ',' << point.second << ' ';
            f << "\"/>";
            for (auto point : points) f << "<circle cx=\"" << point.first << "\" cy=\"" << point.second << "\" r=\"4\" fill=\"" << color << "\"/>";
            const int lx = int(x0)+(legend%2)*205, ly = 445+(legend/2)*25;
            f << "<path d=\"M " << lx << ' ' << ly << " h 22\" stroke=\"" << color << "\" stroke-width=\"2\"" << dash << "/>"
              << "<text x=\"" << lx+28 << "\" y=\"" << ly+4 << "\" font-size=\"11\">" << "think=" << think << " ms</text>";
            ++legend;
        }
    }
    f << "<text x=\"550\" y=\"510\" text-anchor=\"middle\" font-size=\"12\">Means of three real runs per configuration</text></g></svg>";
    f.close(); if (!f) throw std::runtime_error("Cannot write graph");
}
int experiment_suite() {
    namespace fs = std::filesystem;
    const fs::path folder = "results_" + std::to_string(GetTickCount64());
    if (!fs::create_directory(folder)) throw std::runtime_error("Cannot create results directory");
    std::ofstream runs(folder/"runs.csv"), summary(folder/"summary.csv");
    if (!runs || !summary) throw std::runtime_error("Cannot create CSV files");
    runs << "preset,philosophers,cycles,seed,think_ms,repeat,elapsed_ms,throughput_ops_s,mean_wait_ms,max_wait_ms,completed,timeouts,violations\n";
    summary << "preset,philosophers,think_ms,run1_ms,run2_ms,run3_ms,mean_ms,mean_throughput_ops_s,mean_wait_ms,max_wait_ms,timeouts,violations\n";
    runs << std::fixed << std::setprecision(6);
    summary << std::fixed << std::setprecision(6);
    std::vector<ExperimentRow> rows;
    for (int individual : {0,1}) for (int n : {5,7}) for (int think : {2,15}) {
        double times[3]{}, sum_time=0, sum_q=0, sum_wait=0, max_wait=0;
        int timeouts=0, violations=0;
        const std::string preset = individual ? "individual" : "standard";
        for (int repeat=1; repeat<=3; ++repeat) {
            Config c;
            c.cycles=individual?235:100; c.seed=individual?3619:1000;
            c.philosophers=n; c.think=think; c.quiet=true;
            std::cout << "\nRun " << rows.size()+1 << "/24: " << preset
                      << " N=" << n << " THINK=" << think << " repeat=" << repeat << '\n';
            Measurement m;
            if (execute(c, &m) != 0)
                throw std::runtime_error("Experiment failed; partial CSV saved, graphs not generated");
            rows.push_back({preset,n,think,repeat,m});
            runs << preset << ',' << n << ',' << c.cycles << ',' << c.seed << ','
                 << think << ',' << repeat << ',' << m.elapsed << ',' << m.throughput << ','
                 << m.mean_wait << ',' << m.max_wait << ',' << m.completed << ','
                 << m.timeouts << ',' << m.violations << '\n';
            times[repeat-1]=m.elapsed; sum_time+=m.elapsed;
            sum_q+=m.throughput; sum_wait+=m.mean_wait;
            max_wait=std::max(max_wait,m.max_wait);
            timeouts+=m.timeouts; violations+=m.violations;
        }
        summary << preset << ',' << n << ',' << think << ','
                << times[0] << ',' << times[1] << ',' << times[2] << ',' << sum_time/3 << ','
                << sum_q/3 << ',' << sum_wait/3 << ',' << max_wait << ','
                << timeouts << ',' << violations << '\n';
    }
    runs.close(); summary.close();
    if (!runs || !summary) throw std::runtime_error("Cannot write CSV files");
    chart(folder/"throughput.svg",rows,false);
    chart(folder/"waiting.svg",rows,true);
    std::cout << "\n24 runs completed. CSV tables and two SVG graphs:\n"
              << fs::absolute(folder).string() << '\n';
    return 0;
}
int main() {
    try {
        std::cout << "LAB 4 | VARIANT 4 | STUDENT_ID=027\n"
                  << "1. Standard experiment (100 cycles, seed=1000)\n"
                  << "2. Individual experiment (235 cycles, seed=3619)\n"
                  << "3. Custom parameters\n"
                  << "4. All 24 experiments + CSV + two graphs\n"
                  << "0. Exit\n";
        const int choice=input_number("Choice",1,0,4);
        if (choice==0) return 0;
        if (choice==4) return experiment_suite();
        Config c;
        if (choice==2) { c.cycles=235; c.seed=3619; }
        if (choice==3) {
            c.philosophers=input_number("Philosophers",5,2,32);
            c.cycles=input_number("Cycles per philosopher",100,1,1000000);
            c.think=input_number("Thinking time ms",2,0,10000);
            c.eat=input_number("Maximum eating time ms (random 1..value)",2,1,10000);
            c.seed=static_cast<uint32_t>(input_number("Seed",3619,0,2147483647));
            c.timeout=static_cast<uint32_t>(input_number("Pair acquisition timeout ms",1000,1,60000));
            c.duration=static_cast<uint32_t>(input_number("Maximum experiment duration ms",60000,1,3600000));
        }
        c.quiet=input_number("Detailed log: 1=yes, 0=no",1,0,1)==0;
        return execute(c);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
