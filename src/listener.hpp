// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "event_loop.hpp"
#include "socket.hpp"

#include <cstdint>
#include <functional>

#include <netinet/in.h>

namespace mm {

/// Owns the TCP listening socket. On each incoming connection it invokes the
/// accept callback with the accepted (nonblocking) socket and the peer address.
class listener : public io_handler {
public:
    using accept_fn = std::function<void(socket, const sockaddr_in&)>;

    listener(event_loop& loop, accept_fn onAccept);
    ~listener() override;

    /// Bind to bindAddr:port and listen. `port == 0` requests an ephemeral
    /// port; the resolved port is available via bound_port() afterwards. Throws
    /// std::system_error on failure.
    void start(const std::string& bindAddr, std::uint16_t port);

    [[nodiscard]] std::uint16_t bound_port() const noexcept { return boundPort_; }

    void on_io_events(std::uint32_t events) override;

private:
    event_loop&    loop_;
    accept_fn      onAccept_;
    socket        sock_;
    std::uint16_t boundPort_ = 0;
};

} // namespace mm
