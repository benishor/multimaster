// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "discovery.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>

namespace mm {

discovery::discovery(event_loop& loop, const mesh_config& cfg, const local_identity& self,
                     discover_fn onDiscover, error_fn onError)
    : loop_(loop), cfg_(cfg), self_(self), onDiscover_(std::move(onDiscover)),
      onError_(std::move(onError)), rng_(std::random_device{}()) {}

discovery::~discovery() {
    if (sock_.valid()) loop_.del(sock_.get());
}

void discovery::start() {
    auto report = [&](const char* what) {
        if (onError_) onError_({error_category::Discovery, errno, what, std::nullopt});
    };

    sock_ = socket::udp();
    if (!sock_.valid()) { report("multicast socket()"); return; }
    (void)sock_.set_reuse_addr();
    (void)sock_.set_reuse_port(); // allow multiple nodes on one host (and tests)

    // Bind to the multicast port on any interface.
    sockaddr_in bindAddr{};
    if (!make_addr("0.0.0.0", cfg_.multicastPort, bindAddr) ||
        ::bind(sock_.get(), reinterpret_cast<sockaddr*>(&bindAddr), sizeof bindAddr) != 0) {
        report("multicast bind()");
        return;
    }

    // Join the multicast group.
    ip_mreq mreq{};
    if (::inet_pton(AF_INET, cfg_.multicastAddr.c_str(), &mreq.imr_multiaddr) != 1) {
        report("bad multicast address");
        return;
    }
    if (!cfg_.multicastIface.empty()) {
        ::inet_pton(AF_INET, cfg_.multicastIface.c_str(), &mreq.imr_interface);
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }
    if (::setsockopt(sock_.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
        report("IP_ADD_MEMBERSHIP");
        return;
    }

    // Outbound multicast options: TTL + enable loopback (needed for same-host
    // multi-node and the smoke test).
    unsigned char ttl  = cfg_.multicastTtl;
    unsigned char loop = 1;
    ::setsockopt(sock_.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    ::setsockopt(sock_.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    if (!cfg_.multicastIface.empty()) {
        in_addr iface{};
        ::inet_pton(AF_INET, cfg_.multicastIface.c_str(), &iface);
        ::setsockopt(sock_.get(), IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof iface);
    }

    if (!make_addr(cfg_.multicastAddr, cfg_.multicastPort, groupAddr_)) {
        report("bad multicast dest");
        return;
    }

    if (!loop_.add(sock_.get(), EPOLLIN, this)) { report("multicast epoll add"); return; }

    usable_ = true;
    send_announce();          // announce immediately on startup
    schedule_announce();  // then periodically
}

void discovery::send_announce() {
    if (!usable_) return;
    announce a;
    a.protocolVersion = self_.protocolVersion;
    a.flags           = self_.secure ? kAnnounceFlagSecure : 0;
    a.tcpListenPort   = self_.listenPort;
    a.nodeId          = self_.nodeId;
    a.groupName       = self_.groupName;
    auto dat = encode_announce(a);
    if (self_.secure) {
        // Append a keyed MAC so peers without the PSK cannot forge or enumerate.
        auto tag = crypto::discovery_tag(std::span<const std::byte>(dat.data(), dat.size()),
                                         self_.discoveryKey);
        dat.insert(dat.end(), tag.begin(), tag.end());
    }
    ::sendto(sock_.get(), dat.data(), dat.size(), 0,
             reinterpret_cast<sockaddr*>(&groupAddr_), sizeof groupAddr_);
}

void discovery::schedule_announce() {
    // Jitter ±20% to avoid mesh-wide announce synchronization.
    auto base = cfg_.announceInterval;
    std::uniform_int_distribution<long long> jit(-base.count() / 5, base.count() / 5);
    auto delay = base + std::chrono::milliseconds(jit(rng_));
    loop_.add_timer(delay, [this] {
        send_announce();
        schedule_announce();
    });
}

void discovery::on_io_events(std::uint32_t events) {
    if (!(events & EPOLLIN)) return;
    std::array<std::byte, 2048> buf{};
    for (;;) {
        sockaddr_in src{};
        socklen_t   slen = sizeof src;
        ssize_t n = ::recvfrom(sock_.get(), buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&src), &slen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            break;
        }
        std::span<const std::byte> datagram(buf.data(), static_cast<std::size_t>(n));
        std::span<const std::byte> body = datagram;
        if (self_.secure) {
            // Verify and strip the trailing keyed MAC; drop on any mismatch.
            if (datagram.size() <= 16) continue;
            body          = datagram.first(datagram.size() - 16);
            auto received = datagram.subspan(datagram.size() - 16, 16);
            if (!crypto::discovery_verify(body, received, self_.discoveryKey)) continue;
        }
        auto a = decode_announce(body);
        if (!a) continue;
        // Both ends must agree on whether the mesh is secured.
        if (((a->flags & kAnnounceFlagSecure) != 0) != self_.secure) continue;
        if (a->protocolVersion != self_.protocolVersion) continue;
        if (a->groupName != self_.groupName) continue;
        if (a->nodeId == self_.nodeId) continue; // ignore our own echo
        if (onDiscover_) onDiscover_(*a, src);
    }
}

} // namespace mm
