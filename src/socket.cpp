// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace mm {

void socket::reset() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool socket::set_non_blocking() {
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool socket::set_reuse_addr() {
    int on = 1;
    return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == 0;
}

bool socket::set_reuse_port() {
    int on = 1;
    return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) == 0;
}

bool socket::set_tcp_no_delay() {
    int on = 1;
    return ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) == 0;
}

bool socket::set_keep_alive() {
    int on = 1;
    return ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on) == 0;
}

socket socket::tcp() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return socket(fd);
}

socket socket::udp() {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return socket(fd);
}

socket socket::accept(sockaddr_in& peerAddr) const {
    socklen_t len = sizeof peerAddr;
    int fd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&peerAddr), &len,
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    return socket(fd);
}

bool make_addr(std::string_view ip, uint16_t port, sockaddr_in& out) {
    std::memset(&out, 0, sizeof out);
    out.sin_family = AF_INET;
    out.sin_port   = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
        out.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    std::string tmp(ip); // ensure NUL-terminated for inet_pton
    return ::inet_pton(AF_INET, tmp.c_str(), &out.sin_addr) == 1;
}

std::string ip_to_string(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf);
    return buf;
}

} // namespace mm
