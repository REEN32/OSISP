#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#if defined(_WIN32) && !defined(LAB_PORTABLE)
#define NOMINMAX
#include <windows.h>
#include <process.h>
#else
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#ifdef __APPLE__
#include <pthread.h>
#endif
#endif

namespace os {
#if defined(_WIN32) && !defined(LAB_PORTABLE)
inline constexpr const char* backend = "windows-api";
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
    Thread(Entry entry, void* arg): h_(reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, entry, arg, 0, nullptr))) {}
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
#else
inline constexpr const char* backend = "portable-validation";
class Event {
    std::mutex m_; std::condition_variable cv_; bool signaled_ = false;
public:
    void set() { { std::lock_guard<std::mutex> g(m_); signaled_ = true; } cv_.notify_all(); }
    bool wait(uint32_t ms) {
        std::unique_lock<std::mutex> g(m_);
        return cv_.wait_for(g, std::chrono::milliseconds(ms), [&] { return signaled_; });
    }
};
class Mutex {
    std::timed_mutex m_;
public:
    bool acquire(Event& stop, uint32_t ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        do {
            if (stop.wait(0)) return false;
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (m_.try_lock_for(std::chrono::milliseconds(left > 10 ? 10 : (left > 0 ? left : 0)))) return true;
        } while (std::chrono::steady_clock::now() < end);
        return false;
    }
    void release() { m_.unlock(); }
};
using Lock = std::mutex;
using Entry = unsigned (*)(void*);
class Thread {
    std::thread t_;
public:
    Thread(Entry entry, void* arg): t_([=] { entry(arg); }) {}
    void join() { t_.join(); }
};
inline uint64_t tid() {
#ifdef __APPLE__
    uint64_t id = 0; pthread_threadid_np(nullptr, &id); return id;
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}
inline double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#define THREAD_ENTRY unsigned
#endif
class Guard {
    Lock& l_;
public:
    explicit Guard(Lock& l): l_(l) { l_.lock(); }
    ~Guard() { l_.unlock(); }
    Guard(const Guard&) = delete;
};
}
