#include "Listener.hpp"

#include <cerrno>
#include <cstring>
#include <system_error>

#include <arpa/inet.h>
#include <sys/socket.h>

namespace mm {

Listener::Listener(EventLoop& loop, AcceptFn onAccept)
    : loop_(loop), onAccept_(std::move(onAccept)) {}

Listener::~Listener() {
    if (sock_.valid()) loop_.del(sock_.get());
}

void Listener::start(const std::string& bindAddr, std::uint16_t port) {
    sock_ = Socket::tcp();
    if (!sock_.valid()) {
        throw std::system_error(errno, std::generic_category(), "listener socket()");
    }
    (void)sock_.setReuseAddr();

    sockaddr_in addr{};
    if (!makeAddr(bindAddr, port, addr)) {
        throw std::system_error(EINVAL, std::generic_category(), "bad bind address");
    }
    if (::bind(sock_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        throw std::system_error(errno, std::generic_category(), "listener bind()");
    }
    if (::listen(sock_.get(), 64) != 0) {
        throw std::system_error(errno, std::generic_category(), "listen()");
    }

    // Resolve the actual bound port (matters when port == 0).
    sockaddr_in bound{};
    socklen_t   len = sizeof bound;
    if (::getsockname(sock_.get(), reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        boundPort_ = ntohs(bound.sin_port);
    }

    if (!loop_.add(sock_.get(), EPOLLIN, this)) {
        throw std::system_error(errno, std::generic_category(), "listener epoll add");
    }
}

void Listener::onIoEvents(std::uint32_t events) {
    if (!(events & EPOLLIN)) return;
    for (;;) {
        sockaddr_in peer{};
        Socket conn = sock_.accept(peer);
        if (!conn.valid()) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break; // transient; try again next readiness
        }
        onAccept_(std::move(conn), peer);
    }
}

} // namespace mm
