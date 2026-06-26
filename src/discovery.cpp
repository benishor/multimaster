#include "discovery.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>

namespace mm {

Discovery::Discovery(EventLoop& loop, const MeshConfig& cfg, const LocalIdentity& self,
                     DiscoverFn onDiscover, ErrorFn onError)
    : loop_(loop), cfg_(cfg), self_(self), onDiscover_(std::move(onDiscover)),
      onError_(std::move(onError)), rng_(std::random_device{}()) {}

Discovery::~Discovery() {
    if (sock_.valid()) loop_.del(sock_.get());
}

void Discovery::start() {
    auto report = [&](const char* what) {
        if (onError_) onError_({ErrorCategory::Discovery, errno, what, std::nullopt});
    };

    sock_ = Socket::udp();
    if (!sock_.valid()) { report("multicast socket()"); return; }
    (void)sock_.setReuseAddr();
    (void)sock_.setReusePort(); // allow multiple nodes on one host (and tests)

    // Bind to the multicast port on any interface.
    sockaddr_in bindAddr{};
    if (!makeAddr("0.0.0.0", cfg_.multicastPort, bindAddr) ||
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

    if (!makeAddr(cfg_.multicastAddr, cfg_.multicastPort, groupAddr_)) {
        report("bad multicast dest");
        return;
    }

    if (!loop_.add(sock_.get(), EPOLLIN, this)) { report("multicast epoll add"); return; }

    usable_ = true;
    announce();          // announce immediately on startup
    scheduleAnnounce();  // then periodically
}

void Discovery::announce() {
    if (!usable_) return;
    Announce a;
    a.protocolVersion = self_.protocolVersion;
    a.flags           = 0;
    a.tcpListenPort   = self_.listenPort;
    a.nodeId          = self_.nodeId;
    a.groupName       = self_.groupName;
    auto dat = encodeAnnounce(a);
    ::sendto(sock_.get(), dat.data(), dat.size(), 0,
             reinterpret_cast<sockaddr*>(&groupAddr_), sizeof groupAddr_);
}

void Discovery::scheduleAnnounce() {
    // Jitter ±20% to avoid mesh-wide announce synchronization.
    auto base = cfg_.announceInterval;
    std::uniform_int_distribution<long long> jit(-base.count() / 5, base.count() / 5);
    auto delay = base + std::chrono::milliseconds(jit(rng_));
    loop_.addTimer(delay, [this] {
        announce();
        scheduleAnnounce();
    });
}

void Discovery::onIoEvents(std::uint32_t events) {
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
        auto a = decodeAnnounce(std::span<const std::byte>(buf.data(), static_cast<std::size_t>(n)));
        if (!a) continue;
        if (a->protocolVersion != self_.protocolVersion) continue;
        if (a->groupName != self_.groupName) continue;
        if (a->nodeId == self_.nodeId) continue; // ignore our own echo
        if (onDiscover_) onDiscover_(*a, src);
    }
}

} // namespace mm
