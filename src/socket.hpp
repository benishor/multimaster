// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace mm {

/// Move-only RAII wrapper over a file descriptor. Closes on destruction.
/// All factory functions create sockets with SOCK_CLOEXEC set.
class socket {
 public:
  socket() = default;
  explicit socket(int fd) : fd_(fd) {}
  ~socket() { reset(); }

  socket(socket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  socket& operator=(socket&& o) noexcept {
    if (this != &o) {
      reset();
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }
  socket(const socket&) = delete;
  socket& operator=(const socket&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  /// Relinquish ownership of the fd without closing it.
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

  void reset() noexcept;

  // --- fd configuration helpers (return false + leave errno on failure) ---
  [[nodiscard]] bool set_non_blocking();
  [[nodiscard]] bool set_reuse_addr();
  [[nodiscard]] bool set_reuse_port();
  [[nodiscard]] bool set_tcp_no_delay();
  [[nodiscard]] bool set_keep_alive();

  // --- factories ----------------------------------------------------------
  [[nodiscard]] static socket tcp();  // nonblocking TCP socket
  [[nodiscard]] static socket udp();  // nonblocking UDP socket
  /// Accept a connection; returns an invalid socket on EAGAIN/on_error (check
  /// errno). The accepted fd is nonblocking + CLOEXEC.
  [[nodiscard]] socket accept(sockaddr_in& peerAddr) const;

 private:
  int fd_ = -1;
};

// --- address helpers --------------------------------------------------------

/// Build a sockaddr_in. `ip` "" or "0.0.0.0" => INADDR_ANY. Returns false on a
/// malformed IPv4 literal.
[[nodiscard]] bool make_addr(std::string_view ip, uint16_t port,
                             sockaddr_in& out);

/// Dotted-quad string for the address part of `addr`.
[[nodiscard]] std::string ip_to_string(const sockaddr_in& addr);

}  // namespace mm
