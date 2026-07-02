// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <sys/epoll.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_set>

#include "socket.hpp"

namespace mm {

/// Implemented by anything that owns an fd registered with the event_loop. The
/// loop calls on_io_events() with the epoll event mask
/// (EPOLLIN/OUT/HUP/ERR/...).
class io_handler {
 public:
  virtual ~io_handler() = default;
  virtual void on_io_events(std::uint32_t events) = 0;
};

/// Single-threaded epoll reactor with a steady_clock timer queue and a
/// cross-thread wakeup (eventfd). All methods except wakeup() and stop() must
/// be called on the loop's own thread (i.e. from within run() / handlers /
/// timers). wakeup() and stop() are safe to call from any thread.
class event_loop {
 public:
  using clock = std::chrono::steady_clock;
  using timer_id = std::uint64_t;

  event_loop();
  ~event_loop();

  event_loop(const event_loop&) = delete;
  event_loop& operator=(const event_loop&) = delete;

  // fd registration. Return false on epoll_ctl failure (errno set).
  bool add(int fd, std::uint32_t events, io_handler* h);
  bool mod(int fd, std::uint32_t events, io_handler* h);
  bool del(int fd);

  /// Schedule a one-shot callback after `delay`. Re-arm from inside the
  /// callback for periodic behavior. Returns an id usable with cancel_timer().
  timer_id add_timer(clock::duration delay, std::function<void()> cb);
  void cancel_timer(timer_id);

  /// Set the handler invoked (on the loop thread) after a cross-thread
  /// wakeup() call. Used by mesh_impl to drain its command mailbox.
  void set_wake_handler(std::function<void()> h) {
    wakeHandler_ = std::move(h);
  }

  /// Wake the loop from another thread (e.g. to process a posted command or
  /// to observe stop()). Coalesces; many calls => at most one extra cycle.
  void wakeup();

  /// Run until stop() is observed.
  void run();

  /// Request the loop to exit after the current cycle. Thread-safe.
  void stop();

 private:
  void drain_wake();
  int next_timeout_ms();
  void fire_due_timers();

  socket epollFd_;
  socket eventFd_;

  struct timer {
    timer_id id;
    std::function<void()> cb;
  };
  std::multimap<clock::time_point, timer> timers_;
  std::unordered_set<timer_id> cancelled_;
  timer_id nextTimerId_ = 1;

  std::function<void()> wakeHandler_;
  volatile bool running_ = false;
};

}  // namespace mm
