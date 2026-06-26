#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <netinet/in.h>

namespace mm {

/// Move-only RAII wrapper over a file descriptor. Closes on destruction.
/// All factory functions create sockets with SOCK_CLOEXEC set.
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { reset(); }

    Socket(Socket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    Socket(const Socket&)            = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int  get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    /// Relinquish ownership of the fd without closing it.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    void reset() noexcept;

    // --- fd configuration helpers (return false + leave errno on failure) ---
    [[nodiscard]] bool setNonBlocking();
    [[nodiscard]] bool setReuseAddr();
    [[nodiscard]] bool setReusePort();
    [[nodiscard]] bool setTcpNoDelay();
    [[nodiscard]] bool setKeepAlive();

    // --- factories ----------------------------------------------------------
    [[nodiscard]] static Socket tcp();        // nonblocking TCP socket
    [[nodiscard]] static Socket udp();        // nonblocking UDP socket
    /// Accept a connection; returns an invalid Socket on EAGAIN/error (check
    /// errno). The accepted fd is nonblocking + CLOEXEC.
    [[nodiscard]] Socket accept(sockaddr_in& peerAddr) const;

private:
    int fd_ = -1;
};

// --- address helpers --------------------------------------------------------

/// Build a sockaddr_in. `ip` "" or "0.0.0.0" => INADDR_ANY. Returns false on a
/// malformed IPv4 literal.
[[nodiscard]] bool makeAddr(std::string_view ip, uint16_t port, sockaddr_in& out);

/// Dotted-quad string for the address part of `addr`.
[[nodiscard]] std::string ipToString(const sockaddr_in& addr);

} // namespace mm
