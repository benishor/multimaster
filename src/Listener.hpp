#pragma once

#include "EventLoop.hpp"
#include "Socket.hpp"

#include <cstdint>
#include <functional>

#include <netinet/in.h>

namespace mm {

/// Owns the TCP listening socket. On each incoming connection it invokes the
/// accept callback with the accepted (nonblocking) socket and the peer address.
class Listener : public IoHandler {
public:
    using AcceptFn = std::function<void(Socket, const sockaddr_in&)>;

    Listener(EventLoop& loop, AcceptFn onAccept);
    ~Listener() override;

    /// Bind to bindAddr:port and listen. `port == 0` requests an ephemeral
    /// port; the resolved port is available via boundPort() afterwards. Throws
    /// std::system_error on failure.
    void start(const std::string& bindAddr, std::uint16_t port);

    [[nodiscard]] std::uint16_t boundPort() const noexcept { return boundPort_; }

    void onIoEvents(std::uint32_t events) override;

private:
    EventLoop&    loop_;
    AcceptFn      onAccept_;
    Socket        sock_;
    std::uint16_t boundPort_ = 0;
};

} // namespace mm
