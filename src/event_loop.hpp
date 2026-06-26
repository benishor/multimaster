#pragma once

#include "socket.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_set>

#include <sys/epoll.h>

namespace mm {

/// Implemented by anything that owns an fd registered with the EventLoop. The
/// loop calls onIoEvents() with the epoll event mask (EPOLLIN/OUT/HUP/ERR/...).
class IoHandler {
public:
    virtual ~IoHandler() = default;
    virtual void onIoEvents(std::uint32_t events) = 0;
};

/// Single-threaded epoll reactor with a steady_clock timer queue and a
/// cross-thread wakeup (eventfd). All methods except wakeup() and stop() must be
/// called on the loop's own thread (i.e. from within run() / handlers / timers).
/// wakeup() and stop() are safe to call from any thread.
class EventLoop {
public:
    using Clock   = std::chrono::steady_clock;
    using TimerId = std::uint64_t;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&)            = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // fd registration. Return false on epoll_ctl failure (errno set).
    bool add(int fd, std::uint32_t events, IoHandler* h);
    bool mod(int fd, std::uint32_t events, IoHandler* h);
    bool del(int fd);

    /// Schedule a one-shot callback after `delay`. Re-arm from inside the
    /// callback for periodic behavior. Returns an id usable with cancelTimer().
    TimerId addTimer(Clock::duration delay, std::function<void()> cb);
    void    cancelTimer(TimerId);

    /// Set the handler invoked (on the loop thread) after a cross-thread
    /// wakeup() call. Used by MeshImpl to drain its command mailbox.
    void setWakeHandler(std::function<void()> h) { wakeHandler_ = std::move(h); }

    /// Wake the loop from another thread (e.g. to process a posted command or
    /// to observe stop()). Coalesces; many calls => at most one extra cycle.
    void wakeup();

    /// Run until stop() is observed.
    void run();

    /// Request the loop to exit after the current cycle. Thread-safe.
    void stop();

private:
    void drainWake();
    int  nextTimeoutMs();
    void fireDueTimers();

    Socket epollFd_;
    Socket eventFd_;

    struct Timer {
        TimerId               id;
        std::function<void()> cb;
    };
    std::multimap<Clock::time_point, Timer> timers_;
    std::unordered_set<TimerId>             cancelled_;
    TimerId                                 nextTimerId_ = 1;

    std::function<void()> wakeHandler_;
    volatile bool         running_ = false;
};

} // namespace mm
