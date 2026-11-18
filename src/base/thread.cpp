#include "base/thread.h"

#include "base/time.h"

#if defined(MSGA_PORT)
#include "port/port.h"
#elif defined(_WIN32)
#include <windows.h>

#include <chrono>
#include <thread>
#else
#include <pthread.h>

#include <chrono>
#include <thread>
#endif

namespace base {

// ── Locks ───────────────────────────────────────────────────────────────────

int64_t CondVar::nowMs() {
    return monotonicMs();
}

#ifdef MSGA_PORT
namespace {
// The handle of a lazily made port object: made on first use, the loser of
// a race destroys its own.
template <class T>
T *lazy(std::atomic<T *> &slot, int (*create)(T **), void (*destroy)(T *)) {
    T *p = slot.load(std::memory_order_acquire);
    if (p)
        return p;
    T *made = nullptr;
    if (create(&made) != PORT_OK || !made)
        __builtin_trap(); // no lock: nothing can go on correctly
    if (slot.compare_exchange_strong(p, made, std::memory_order_acq_rel))
        return made;
    destroy(made);
    return p;
}
} // namespace

Mutex::~Mutex() {
    port_mutex_destroy(_m.load(std::memory_order_acquire));
}

port_mutex *Mutex::get() {
    return lazy(_m, port_mutex_create, port_mutex_destroy);
}

void Mutex::lock() {
    port_mutex_lock(get());
}

void Mutex::unlock() {
    port_mutex_unlock(get());
}

bool Mutex::try_lock() {
    return port_mutex_trylock(get()) == PORT_OK;
}

CondVar::~CondVar() {
    port_cond_destroy(_c.load(std::memory_order_acquire));
}

port_cond *CondVar::get() {
    return lazy(_c, port_cond_create, port_cond_destroy);
}

void CondVar::wait(Lock &l) {
    port_cond_wait(get(), l._m.get());
}

bool CondVar::waitFor(Lock &l, int ms) {
    return port_cond_wait_ms(get(), l._m.get(), uint32_t(ms < 0 ? 0 : ms)) == PORT_OK;
}

void CondVar::notifyOne() {
    port_cond_signal(get());
}

void CondVar::notifyAll() {
    port_cond_broadcast(get());
}

void sleepMs(int ms) {
    port_sleep_ms(uint32_t(ms < 0 ? 0 : ms));
}

int hardwareThreads() {
    const int n = port_cpu_count();
    return n > 0 ? n : 1;
}
#else
bool CondVar::waitFor(Lock &l, int ms) {
    std::unique_lock<std::mutex> u(l._m._m, std::adopt_lock);
    const bool                   notified =
        _cv.wait_for(u, std::chrono::milliseconds(ms)) == std::cv_status::no_timeout;
    u.release(); // the Lock still holds it
    return notified;
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int hardwareThreads() {
    const int n = int(std::thread::hardware_concurrency());
    return n > 0 ? n : 1;
}
#endif

// ── Thread ──────────────────────────────────────────────────────────────────

namespace {
#if defined(MSGA_PORT)
void trampoline(void *p) {
#elif defined(_WIN32)
DWORD WINAPI trampoline(void *p) {
#else
void *trampoline(void *p) {
#endif
    auto *fn = static_cast<std::function<void()> *>(p);
    (*fn)();
    delete fn;
#ifndef MSGA_PORT
    return 0;
#endif
}
} // namespace

Thread::~Thread() {
    join();
}

bool Thread::start(std::function<void()> fn, size_t stackBytes) {
    if (_handle)
        return false;
    auto *heap = new std::function<void()>(std::move(fn));
#if defined(MSGA_PORT)
    port_thread *t = nullptr;
    if (port_thread_start(&t, trampoline, heap, stackBytes) != PORT_OK) {
        delete heap;
        return false;
    }
    _handle = t;
#elif defined(_WIN32)
    HANDLE h = CreateThread(
        nullptr, stackBytes, trampoline, heap, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr
    );
    if (!h) {
        delete heap;
        return false;
    }
    _handle = h;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackBytes);
    auto     *t  = new pthread_t;
    const int rc = pthread_create(t, &attr, trampoline, heap);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete t;
        delete heap;
        return false;
    }
    _handle = t;
#endif
    return true;
}

void Thread::join() {
    if (!_handle)
        return;
#if defined(MSGA_PORT)
    port_thread_join(static_cast<port_thread *>(_handle));
#elif defined(_WIN32)
    WaitForSingleObject(static_cast<HANDLE>(_handle), INFINITE);
    CloseHandle(static_cast<HANDLE>(_handle));
#else
    auto *t = static_cast<pthread_t *>(_handle);
    pthread_join(*t, nullptr);
    delete t;
#endif
    _handle = nullptr;
}

void Thread::detach() {
    if (!_handle)
        return;
#if defined(MSGA_PORT)
    port_thread_detach(static_cast<port_thread *>(_handle));
#elif defined(_WIN32)
    CloseHandle(static_cast<HANDLE>(_handle));
#else
    auto *t = static_cast<pthread_t *>(_handle);
    pthread_detach(*t);
    delete t;
#endif
    _handle = nullptr;
}

// ── WorkerPool ──────────────────────────────────────────────────────────────

struct WorkerPool::Worker {
    Thread thread;
};

WorkerPool::WorkerPool(const Options &o) : _opt(o), _idleMs(o.idleMs) {}

WorkerPool::~WorkerPool() {
    stop();
}

void WorkerPool::loop(Worker *self) {
    Lock lock(_mutex);
    for (;;) {
        if (_queue.empty() && !_stopping && _parked < _opt.maxParked) {
            ++_parked;
            const int64_t since = monotonicMs();
            for (;;) { // the idle time may change while it waits
                const int64_t left = since + _idleMs - monotonicMs();
                if (!_queue.empty() || _stopping || left <= 0)
                    break;
                _wake.waitFor(lock, int(left));
            }
            --_parked;
        }
        if (_stopping)
            return; // stop() joins it
        if (_queue.empty()) {
            // Idle long enough, or enough are parked: it ends. Detached, it
            // touches nothing of the pool once the lock is released.
            for (size_t i = 0; i < _workers.size(); ++i)
                if (_workers[i].get() == self) {
                    self->thread.detach();
                    _workers.erase(_workers.begin() + ptrdiff_t(i));
                    break;
                }
            return;
        }
        std::function<void()> task = std::move(_queue.front());
        _queue.pop_front();
        lock.unlock();
        task();
        task = nullptr; // what it held goes before the next one, outside the lock
        lock.lock();
    }
}

void WorkerPool::post(std::function<void()> task, bool front) {
    {
        Lock lock(_mutex);
        if (_stopping)
            return; // `task` goes with the call, outside the lock
        if (front)
            _queue.push_front(std::move(task));
        else
            _queue.push_back(std::move(task));
        // More queued than the parked workers can take: another worker, up
        // to the cap (then the task waits for one to finish).
        if (int(_queue.size()) > _parked && int(_workers.size()) < _opt.maxWorkers) {
            _workers.push_back(std::make_unique<Worker>());
            Worker *w = _workers.back().get();
            if (w->thread.start([this, w] { loop(w); }, _opt.stackBytes))
                ++_started;
            else
                _workers.pop_back();
        }
    }
    _wake.notifyOne();
}

void WorkerPool::stop() {
    std::deque<std::function<void()>>    dropped;
    std::vector<std::unique_ptr<Worker>> workers;
    {
        Lock lock(_mutex);
        _stopping = true;
        dropped.swap(_queue);
        workers.swap(_workers);
    }
    _wake.notifyAll();
    dropped.clear();
    workers.clear(); // joins each, after its task under way
}

void WorkerPool::setIdleMs(int ms) {
    {
        Lock lock(_mutex);
        _idleMs = ms;
    }
    _wake.notifyAll();
}

WorkerPool::Stats WorkerPool::stats() const {
    Lock lock(_mutex);
    return {int(_workers.size()), _parked, _started};
}

} // namespace base
