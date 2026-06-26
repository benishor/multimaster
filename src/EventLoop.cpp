#include "EventLoop.hpp"

#include <array>
#include <cerrno>
#include <cstdint>

#include <sys/eventfd.h>
#include <unistd.h>

namespace mm {

namespace {
// Sentinel ptr stored in the eventfd's epoll registration so the run loop can
// distinguish a wakeup from a real IoHandler.
IoHandler* const kWakeSentinel = reinterpret_cast<IoHandler*>(1);
} // namespace

EventLoop::EventLoop() {
    epollFd_ = Socket(::epoll_create1(EPOLL_CLOEXEC));
    eventFd_ = Socket(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));

    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.ptr = kWakeSentinel;
    ::epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, eventFd_.get(), &ev);
}

EventLoop::~EventLoop() = default;

bool EventLoop::add(int fd, std::uint32_t events, IoHandler* h) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = h;
    return ::epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EventLoop::mod(int fd, std::uint32_t events, IoHandler* h) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = h;
    return ::epoll_ctl(epollFd_.get(), EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool EventLoop::del(int fd) {
    return ::epoll_ctl(epollFd_.get(), EPOLL_CTL_DEL, fd, nullptr) == 0;
}

EventLoop::TimerId EventLoop::addTimer(Clock::duration delay, std::function<void()> cb) {
    TimerId id = nextTimerId_++;
    timers_.emplace(Clock::now() + delay, Timer{id, std::move(cb)});
    return id;
}

void EventLoop::cancelTimer(TimerId id) { cancelled_.insert(id); }

void EventLoop::wakeup() {
    std::uint64_t one = 1;
    ssize_t n = ::write(eventFd_.get(), &one, sizeof one);
    (void)n; // EAGAIN here just means a wakeup is already pending — fine.
}

void EventLoop::stop() {
    running_ = false;
    wakeup();
}

void EventLoop::drainWake() {
    std::uint64_t buf;
    while (::read(eventFd_.get(), &buf, sizeof buf) == sizeof buf) {
        // drain the counter
    }
}

int EventLoop::nextTimeoutMs() {
    if (timers_.empty()) return -1; // block indefinitely
    auto now  = Clock::now();
    auto next = timers_.begin()->first;
    if (next <= now) return 0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
    if (ms < 0) return 0;
    if (ms > 1000) return 1000; // cap so cancellations/new timers are noticed promptly
    return static_cast<int>(ms);
}

void EventLoop::fireDueTimers() {
    auto now = Clock::now();
    while (!timers_.empty() && timers_.begin()->first <= now) {
        Timer t = std::move(timers_.begin()->second);
        timers_.erase(timers_.begin());
        if (cancelled_.erase(t.id)) continue; // was cancelled
        if (t.cb) t.cb();
        now = Clock::now(); // a timer cb may have taken time / re-armed
    }
}

void EventLoop::run() {
    running_ = true;
    std::array<epoll_event, 64> events{};

    while (running_) {
        int timeout = nextTimeoutMs();
        int n = ::epoll_wait(epollFd_.get(), events.data(),
                             static_cast<int>(events.size()), timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            break; // unrecoverable epoll error
        }

        for (int i = 0; i < n; ++i) {
            // A wake handler (e.g. a Stop command) may tear down handlers whose
            // events are later in this same batch; bail out before touching them.
            if (!running_) break;
            auto* ptr = static_cast<IoHandler*>(events[i].data.ptr);
            if (ptr == kWakeSentinel) {
                drainWake();
                if (wakeHandler_) wakeHandler_();
                continue;
            }
            if (ptr) ptr->onIoEvents(events[i].events);
        }

        fireDueTimers();
    }
}

} // namespace mm
