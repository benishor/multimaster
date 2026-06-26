#include "peer_connection.hpp"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

namespace mm {

PeerConnection::PeerConnection(EventLoop& loop, ConnectionListener& listener,
                               const MeshConfig& cfg, const LocalIdentity& self,
                               Socket sock, const sockaddr_in& peerAddr,
                               std::uint64_t nonce)
    : loop_(loop), listener_(listener), cfg_(cfg), self_(self),
      sock_(std::move(sock)), peerAddr_(peerAddr), inbound_(true),
      state_(State::Handshaking), localNonce_(nonce),
      createdAt_(EventLoop::Clock::now()), lastRecv_(createdAt_), lastSend_(createdAt_) {
    (void)sock_.setTcpNoDelay();
    (void)sock_.setKeepAlive();
    if (loop_.add(sock_.get(), EPOLLIN, this)) registered_ = true;
    sendHello(); // symmetric handshake: announce ourselves immediately
}

PeerConnection::PeerConnection(EventLoop& loop, ConnectionListener& listener,
                               const MeshConfig& cfg, const LocalIdentity& self,
                               const sockaddr_in& target, std::uint64_t nonce)
    : loop_(loop), listener_(listener), cfg_(cfg), self_(self),
      peerAddr_(target), inbound_(false), state_(State::ConnectingOut),
      localNonce_(nonce), createdAt_(EventLoop::Clock::now()),
      lastRecv_(createdAt_), lastSend_(createdAt_) {}

PeerConnection::~PeerConnection() {
    if (registered_ && sock_.valid()) loop_.del(sock_.get());
}

bool PeerConnection::startConnect() {
    sock_ = Socket::tcp();
    if (!sock_.valid()) {
        fail(ErrorCategory::Socket, errno, "socket() failed");
        return false;
    }
    (void)sock_.setTcpNoDelay();
    (void)sock_.setKeepAlive();

    int rc = ::connect(sock_.get(), reinterpret_cast<sockaddr*>(&peerAddr_),
                       sizeof peerAddr_);
    if (rc != 0 && errno != EINPROGRESS) {
        fail(ErrorCategory::Socket, errno, "connect() failed");
        return false;
    }
    // Wait for writability to learn the connect result.
    if (!loop_.add(sock_.get(), EPOLLOUT, this)) {
        fail(ErrorCategory::Socket, errno, "epoll add failed");
        return false;
    }
    registered_ = true;
    wantWrite_  = true;
    return true;
}

void PeerConnection::onConnectComplete() {
    int       err = 0;
    socklen_t len = sizeof err;
    if (::getsockopt(sock_.get(), SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        fail(ErrorCategory::Socket, err, "async connect failed");
        return;
    }
    state_ = State::Handshaking;
    // The socket was registered EPOLLOUT-only for the connect; now we must also
    // listen for the peer's Hello. Re-register explicitly (don't rely on
    // updateEpoll's stale-flag fast path, which can't add EPOLLIN here).
    if (registered_ && loop_.mod(sock_.get(), EPOLLIN, this)) wantWrite_ = false;
    sendHello();   // may re-arm EPOLLOUT via flushOutbound if the write blocks
}

void PeerConnection::sendHello() {
    Hello h;
    h.nodeId          = self_.nodeId;
    h.protocolVersion = self_.protocolVersion;
    h.tcpListenPort   = self_.listenPort;
    h.groupName       = self_.groupName;
    h.nonce           = localNonce_;
    auto frame = encodeHello(inbound_ ? FrameType::HelloAck : FrameType::Hello, h);
    out_.append(frame.data(), frame.size());
    lastSend_ = EventLoop::Clock::now();
    flushOutbound();
}

void PeerConnection::onIoEvents(std::uint32_t events) {
    if (dead_) return;

    if (events & (EPOLLHUP | EPOLLERR)) {
        fail(ErrorCategory::Socket, 0, "peer hangup/error");
        return;
    }

    if (state_ == State::ConnectingOut && (events & EPOLLOUT)) {
        onConnectComplete();
        if (dead_) return;
    }

    if (events & EPOLLOUT) {
        flushOutbound();
        if (dead_) return;
    }

    if (events & (EPOLLIN | EPOLLRDHUP)) {
        doRead();
    }
}

void PeerConnection::doRead() {
    for (;;) {
        std::byte* tail = in_.reserveTail(64 * 1024);
        ssize_t n = ::recv(sock_.get(), tail, 64 * 1024, 0);
        if (n > 0) {
            in_.uncommit(64 * 1024 - static_cast<std::size_t>(n));
            continue; // keep draining until EAGAIN
        }
        in_.uncommit(64 * 1024); // nothing read into the reserved tail
        if (n == 0) {
            fail(ErrorCategory::Socket, 0, "peer closed connection");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        fail(ErrorCategory::Socket, errno, "recv failed");
        return;
    }
    parseInbound();
}

void PeerConnection::parseInbound() {
    for (;;) {
        ParsedFrame frame;
        std::size_t consumed = 0;
        std::span<const std::byte> view(in_.data(), in_.size());
        DecodeStatus st = tryDecodeFrame(view, cfg_.maxMessageBytes, frame, consumed);
        if (st == DecodeStatus::NeedMore) break;
        if (st == DecodeStatus::Error) {
            fail(ErrorCategory::Protocol, 0, "malformed/oversized frame");
            return;
        }

        lastRecv_ = EventLoop::Clock::now();

        switch (frame.type) {
        case FrameType::Hello:
        case FrameType::HelloAck: {
            if (state_ != State::Handshaking) {
                // duplicate Hello — ignore, but still consume
                break;
            }
            const Hello& h = frame.hello;
            if (h.protocolVersion != self_.protocolVersion || h.groupName != self_.groupName) {
                fail(ErrorCategory::Protocol, 0, "protocol/group mismatch");
                return;
            }
            peerId_         = h.nodeId;
            peerListenPort_ = h.tcpListenPort;
            peerNonce_      = h.nonce;
            state_          = State::Established;
            in_.consume(consumed);
            listener_.onPeerHandshake(*this, h);
            if (dead_) return; // listener may have closed us (dial-race loser)
            continue;
        }
        case FrameType::Heartbeat:
            break; // lastRecv_ already updated
        case FrameType::Goodbye:
            in_.consume(consumed);
            fail(ErrorCategory::Socket, 0, "peer said goodbye");
            return;
        case FrameType::Data:
            if (state_ != State::Established) {
                fail(ErrorCategory::Protocol, 0, "data before handshake");
                return;
            }
            // Deliver BEFORE consuming: frame.data.payload views the inbound
            // buffer, which consume() may compact/clear.
            listener_.onPeerData(*this, frame.data);
            if (dead_) return;
            in_.consume(consumed);
            continue;
        }
        in_.consume(consumed);
    }
}

void PeerConnection::sendRaw(std::span<const std::byte> frame) {
    if (dead_) return;
    if (out_.size() + frame.size() > cfg_.maxOutboundQueueBytes) {
        switch (cfg_.overflowPolicy) {
        case MeshConfig::Overflow::Disconnect:
            fail(ErrorCategory::Backpressure, 0, "outbound queue overflow");
            return;
        case MeshConfig::Overflow::DropNewest:
            listener_.reportError({ErrorCategory::Backpressure, 0,
                                   "dropped outbound (newest)", peerId_});
            return;
        case MeshConfig::Overflow::DropOldest:
            // Drop from the head until the new frame fits.
            while (!out_.empty() && out_.size() + frame.size() > cfg_.maxOutboundQueueBytes) {
                out_.consume(out_.size()); // coarse: clear backlog
            }
            break;
        }
    }
    out_.append(frame.data(), frame.size());
    lastSend_ = EventLoop::Clock::now();
    flushOutbound();
}

void PeerConnection::flushOutbound() {
    while (!out_.empty()) {
        ssize_t n = ::send(sock_.get(), out_.data(), out_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out_.consume(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        fail(ErrorCategory::Socket, errno, "send failed");
        return;
    }
    updateEpoll();
}

void PeerConnection::updateEpoll() {
    if (dead_ || !registered_) return;
    bool needWrite = !out_.empty() || state_ == State::ConnectingOut;
    if (needWrite == wantWrite_) return;
    std::uint32_t ev = EPOLLIN | (needWrite ? EPOLLOUT : 0u);
    if (loop_.mod(sock_.get(), ev, this)) wantWrite_ = needWrite;
}

void PeerConnection::maybeHeartbeat(EventLoop::Clock::time_point now,
                                    std::chrono::milliseconds interval) {
    if (state_ != State::Established || dead_) return;
    if (now - lastSend_ >= interval) {
        auto hb = encodeHeartbeat();
        sendRaw(std::span<const std::byte>(hb.data(), hb.size()));
    }
}

void PeerConnection::closeGracefully() {
    if (dead_) return;
    auto bye = encodeGoodbye();
    out_.append(bye.data(), bye.size());
    flushOutbound();
    markDead();
}

void PeerConnection::fail(ErrorCategory cat, int err, std::string what) {
    if (dead_) return;
    if (cat != ErrorCategory::Socket || err != 0) {
        listener_.reportError({cat, err, std::move(what),
                               peerId_.isZero() ? std::nullopt : std::optional<PeerId>(peerId_)});
    }
    markDead();
}

void PeerConnection::markDead() {
    if (dead_) return;
    dead_  = true;
    state_ = State::Dead;
    if (registered_ && sock_.valid()) {
        loop_.del(sock_.get());
        registered_ = false;
    }
    listener_.onPeerClosed(*this); // owner schedules deferred reap
}

} // namespace mm
