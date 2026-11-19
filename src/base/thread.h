// Threads: one with a stack size of our own, a pool of parked workers, and
// the locks, condition variables and sleeps every other file uses, so that
// a port (src/port, MSGA_PORT) provides threads in exactly one place.
//
// Why a stack size of our own: std::thread takes the platform's default,
// and those differ wildly — glibc reserves RLIMIT_STACK (usually 8 MiB),
// Windows the executable's reserve, a dynamically linked musl only 128 KiB
// (too tight for a TLS handshake plus certificate parsing, or a raster fill:
// issue #70). The static musl release links with -z stack-size (8 MiB), so
// std::thread is safe there. An explicit size makes every build behave the
// same and bounds the address space each worker reserves; the pages a thread
// touches are what it costs either way.
//
// Mutex and CondVar are std::mutex / std::condition_variable on the native
// builds (inline: nothing is added), and the port's port_mutex / port_cond
// in a port build, created on first use. A global Mutex then needs no
// constructor to run, which matters on a C runtime that never runs C++
// static initialisers (PEOS has no .init_array).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#ifdef MSGA_PORT
#include <atomic>
struct port_mutex;
struct port_cond;
#else
#include <condition_variable>
#include <mutex>
#endif

namespace base {

class CondVar;

// A non-recursive lock. lock/unlock/try_lock are spelt as the standard's
// BasicLockable, so std::lock_guard / std::unique_lock work with it too.
class Mutex {
public:
    Mutex() = default; // constexpr where its member is: no constructor runs for a global
    Mutex(const Mutex &)            = delete;
    Mutex &operator=(const Mutex &) = delete;
#ifdef MSGA_PORT
    ~Mutex();
    void lock();
    void unlock();
    bool try_lock();

private:
    port_mutex               *get();
    std::atomic<port_mutex *> _m{nullptr};
#else
    void lock() { _m.lock(); }
    void unlock() { _m.unlock(); }
    bool try_lock() { return _m.try_lock(); }

private:
    std::mutex _m;
#endif
    friend class CondVar;
};

// Holds a Mutex for its scope; unlock()/lock() let a caller drop it for a
// stretch (a worker loop runs its task outside the lock).
class Lock {
public:
    explicit Lock(Mutex &m) : _m(m) { _m.lock(); }
    // With onlyIfFree, takes the Mutex only when no one holds it (a caller
    // that must not wait for a worker): held() says whether it did.
    Lock(Mutex &m, bool onlyIfFree) : _m(m), _held(onlyIfFree ? m.try_lock() : (m.lock(), true)) {}
    bool held() const { return _held; }
    ~Lock() {
        if (_held)
            _m.unlock();
    }
    Lock(const Lock &)            = delete;
    Lock &operator=(const Lock &) = delete;
    void  unlock() {
        _m.unlock();
        _held = false;
    }
    void lock() {
        _m.lock();
        _held = true;
    }

private:
    Mutex &_m;
    bool   _held = true;
    friend class CondVar;
};

// Waits on a Lock's Mutex. Waits may wake spuriously: use the predicate
// forms, or re-check in a loop.
class CondVar {
public:
    CondVar()                           = default;
    CondVar(const CondVar &)            = delete;
    CondVar &operator=(const CondVar &) = delete;
#ifdef MSGA_PORT
    ~CondVar();
    void wait(Lock &l);
    void notifyOne();
    void notifyAll();
#else
    void wait(Lock &l) {
        std::unique_lock<std::mutex> u(l._m._m, std::adopt_lock);
        _cv.wait(u);
        u.release(); // the Lock still holds it
    }
    void notifyOne() { _cv.notify_one(); }
    void notifyAll() { _cv.notify_all(); }
#endif
    // False when `ms` passed without a notify; may return true early.
    bool waitFor(Lock &l, int ms);
    template <class Pred>
    void wait(Lock &l, Pred done) {
        while (!done())
            wait(l);
    }
    // done() once true, or false after `ms` (monotonic) with done() false.
    template <class Pred>
    bool waitFor(Lock &l, int ms, Pred done) {
        const int64_t until = nowMs() + ms;
        while (!done()) {
            const int64_t left = until - nowMs();
            if (left <= 0)
                return false;
            waitFor(l, int(left));
        }
        return true;
    }

private:
    static int64_t nowMs(); // base::monotonicMs(), without time.h in every includer
#ifdef MSGA_PORT
    port_cond               *get();
    std::atomic<port_cond *> _c{nullptr};
#else
    std::condition_variable _cv;
#endif
};

// Blocks the calling thread.
void sleepMs(int ms);
// Logical CPUs; at least 1.
int  hardwareThreads();

// Workers' stack unless a caller knows better: room for the recursive
// parsers (JSON nests 256 deep) and the decoders, 8x musl's default.
constexpr size_t kThreadStack = 1024 * 1024;

// A joinable thread.
class Thread {
public:
    Thread() = default;
    ~Thread(); // joins
    Thread(const Thread &)            = delete;
    Thread &operator=(const Thread &) = delete;

    bool start(std::function<void()> fn, size_t stackBytes = kThreadStack);
    void join();
    // It runs on unjoined and ends on its own; this object is free again.
    void detach();

private:
    void *_handle = nullptr; // pthread_t* / HANDLE
};

// Up to maxWorkers threads take posted tasks, oldest first (a front post
// jumps the queue). A task wakes a parked worker, else starts a thread while
// fewer than maxWorkers run; past that it waits for the next free one, so a
// task must never wait on another posted to the same pool. A finished worker
// parks while fewer than maxParked do, else ends; a parked one ends after
// idleMs, so a quiet pool holds no threads.
//
// maxWorkers 1 makes a serial worker: tasks run one at a time, in order.
//
// Destroying the pool (or stop()) drops the queued tasks and joins the
// workers, waiting for the tasks under way. Thread-safe.
class WorkerPool {
public:
    struct Options {
        int    maxWorkers = 4;
        int    maxParked  = 4;
        int    idleMs     = 30'000;
        size_t stackBytes = kThreadStack;
    };
    explicit WorkerPool(const Options &o);
    ~WorkerPool();
    WorkerPool(const WorkerPool &)            = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    // Dropped once stopped. A task is destroyed on its worker, outside any
    // lock of the pool.
    void post(std::function<void()> task, bool front = false);
    void stop();
    // Parked workers measure against the new time (tests shorten it).
    void setIdleMs(int ms);

    // Threads alive, of them parked, threads ever started.
    struct Stats {
        int      workers = 0, parked = 0;
        uint64_t started = 0;
    };
    Stats stats() const;

private:
    struct Worker;
    void loop(Worker *self);

    const Options                        _opt;
    mutable Mutex                        _mutex;
    CondVar                              _wake; // a task was posted, stop, or the idle time changed
    std::deque<std::function<void()>>    _queue;
    std::vector<std::unique_ptr<Worker>> _workers; // alive; a retiring one detaches itself
    int                                  _idleMs;
    int                                  _parked   = 0;
    uint64_t                             _started  = 0;
    bool                                 _stopping = false;
};

} // namespace base
