#include "event_loop.hpp"

#include <array>
#include <cerrno>
#include <cstdint>

#include <sys/eventfd.h>
#include <unistd.h>

namespace mm {

namespace {
// Sentinel ptr stored in the eventfd's epoll registration so the run loop can
// distinguish a wakeup from a real io_handler.
io_handler* const kWakeSentinel = reinterpret_cast<io_handler*>(1);
} // namespace

event_loop::event_loop() {
    epollFd_ = socket(::epoll_create1(EPOLL_CLOEXEC));
    eventFd_ = socket(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));

    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.ptr = kWakeSentinel;
    ::epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, eventFd_.get(), &ev);
}

event_loop::~event_loop() = default;

bool event_loop::add(int fd, std::uint32_t events, io_handler* h) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = h;
    return ::epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool event_loop::mod(int fd, std::uint32_t events, io_handler* h) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = h;
    return ::epoll_ctl(epollFd_.get(), EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool event_loop::del(int fd) {
    return ::epoll_ctl(epollFd_.get(), EPOLL_CTL_DEL, fd, nullptr) == 0;
}

event_loop::timer_id event_loop::add_timer(clock::duration delay, std::function<void()> cb) {
    timer_id id = nextTimerId_++;
    timers_.emplace(clock::now() + delay, timer{id, std::move(cb)});
    return id;
}

void event_loop::cancel_timer(timer_id id) { cancelled_.insert(id); }

void event_loop::wakeup() {
    std::uint64_t one = 1;
    ssize_t n = ::write(eventFd_.get(), &one, sizeof one);
    (void)n; // EAGAIN here just means a wakeup is already pending — fine.
}

void event_loop::stop() {
    running_ = false;
    wakeup();
}

void event_loop::drain_wake() {
    std::uint64_t buf;
    while (::read(eventFd_.get(), &buf, sizeof buf) == sizeof buf) {
        // drain the counter
    }
}

int event_loop::next_timeout_ms() {
    if (timers_.empty()) return -1; // block indefinitely
    auto now  = clock::now();
    auto next = timers_.begin()->first;
    if (next <= now) return 0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
    if (ms < 0) return 0;
    if (ms > 1000) return 1000; // cap so cancellations/new timers are noticed promptly
    return static_cast<int>(ms);
}

void event_loop::fire_due_timers() {
    auto now = clock::now();
    while (!timers_.empty() && timers_.begin()->first <= now) {
        timer t = std::move(timers_.begin()->second);
        timers_.erase(timers_.begin());
        if (cancelled_.erase(t.id)) continue; // was cancelled
        if (t.cb) t.cb();
        now = clock::now(); // a timer cb may have taken time / re-armed
    }
}

void event_loop::run() {
    running_ = true;
    std::array<epoll_event, 64> events{};

    while (running_) {
        int timeout = next_timeout_ms();
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
            auto* ptr = static_cast<io_handler*>(events[i].data.ptr);
            if (ptr == kWakeSentinel) {
                drain_wake();
                if (wakeHandler_) wakeHandler_();
                continue;
            }
            if (ptr) ptr->on_io_events(events[i].events);
        }

        fire_due_timers();
    }
}

} // namespace mm
