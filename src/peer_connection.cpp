#include "peer_connection.hpp"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

namespace mm {

peer_connection::peer_connection(event_loop& loop, connection_listener& listener,
                               const mesh_config& cfg, const local_identity& self,
                               socket sock, const sockaddr_in& peerAddr,
                               std::uint64_t nonce)
    : loop_(loop), listener_(listener), cfg_(cfg), self_(self),
      sock_(std::move(sock)), peerAddr_(peerAddr), inbound_(true),
      state_(conn_state::Handshaking), localNonce_(nonce),
      createdAt_(event_loop::clock::now()), lastRecv_(createdAt_), lastSend_(createdAt_) {
    (void)sock_.set_tcp_no_delay();
    (void)sock_.set_keep_alive();
    if (loop_.add(sock_.get(), EPOLLIN, this)) registered_ = true;
    send_hello(); // symmetric handshake: announce ourselves immediately
}

peer_connection::peer_connection(event_loop& loop, connection_listener& listener,
                               const mesh_config& cfg, const local_identity& self,
                               const sockaddr_in& target, std::uint64_t nonce)
    : loop_(loop), listener_(listener), cfg_(cfg), self_(self),
      peerAddr_(target), inbound_(false), state_(conn_state::ConnectingOut),
      localNonce_(nonce), createdAt_(event_loop::clock::now()),
      lastRecv_(createdAt_), lastSend_(createdAt_) {}

peer_connection::~peer_connection() {
    if (registered_ && sock_.valid()) loop_.del(sock_.get());
}

bool peer_connection::start_connect() {
    sock_ = socket::tcp();
    if (!sock_.valid()) {
        fail(error_category::Socket, errno, "socket() failed");
        return false;
    }
    (void)sock_.set_tcp_no_delay();
    (void)sock_.set_keep_alive();

    int rc = ::connect(sock_.get(), reinterpret_cast<sockaddr*>(&peerAddr_),
                       sizeof peerAddr_);
    if (rc != 0 && errno != EINPROGRESS) {
        fail(error_category::Socket, errno, "connect() failed");
        return false;
    }
    // Wait for writability to learn the connect result.
    if (!loop_.add(sock_.get(), EPOLLOUT, this)) {
        fail(error_category::Socket, errno, "epoll add failed");
        return false;
    }
    registered_ = true;
    wantWrite_  = true;
    return true;
}

void peer_connection::on_connect_complete() {
    int       err = 0;
    socklen_t len = sizeof err;
    if (::getsockopt(sock_.get(), SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        fail(error_category::Socket, err, "async connect failed");
        return;
    }
    state_ = conn_state::Handshaking;
    // The socket was registered EPOLLOUT-only for the connect; now we must also
    // listen for the peer's hello. Re-register explicitly (don't rely on
    // updateEpoll's stale-flag fast path, which can't add EPOLLIN here).
    if (registered_ && loop_.mod(sock_.get(), EPOLLIN, this)) wantWrite_ = false;
    send_hello();   // may re-arm EPOLLOUT via flushOutbound if the write blocks
}

void peer_connection::send_hello() {
    hello h;
    h.nodeId          = self_.nodeId;
    h.protocolVersion = self_.protocolVersion;
    h.tcpListenPort   = self_.listenPort;
    h.groupName       = self_.groupName;
    h.nonce           = localNonce_;
    auto frame = encode_hello(inbound_ ? frame_type::HelloAck : frame_type::Hello, h);
    out_.append(frame.data(), frame.size());
    lastSend_ = event_loop::clock::now();
    flush_outbound();
}

void peer_connection::on_io_events(std::uint32_t events) {
    if (dead_) return;

    if (events & (EPOLLHUP | EPOLLERR)) {
        fail(error_category::Socket, 0, "peer hangup/error");
        return;
    }

    if (state_ == conn_state::ConnectingOut && (events & EPOLLOUT)) {
        on_connect_complete();
        if (dead_) return;
    }

    if (events & EPOLLOUT) {
        flush_outbound();
        if (dead_) return;
    }

    if (events & (EPOLLIN | EPOLLRDHUP)) {
        do_read();
    }
}

void peer_connection::do_read() {
    for (;;) {
        std::byte* tail = in_.reserve_tail(64 * 1024);
        ssize_t n = ::recv(sock_.get(), tail, 64 * 1024, 0);
        if (n > 0) {
            in_.uncommit(64 * 1024 - static_cast<std::size_t>(n));
            continue; // keep draining until EAGAIN
        }
        in_.uncommit(64 * 1024); // nothing read into the reserved tail
        if (n == 0) {
            fail(error_category::Socket, 0, "peer closed connection");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        fail(error_category::Socket, errno, "recv failed");
        return;
    }
    parse_inbound();
}

void peer_connection::parse_inbound() {
    for (;;) {
        parsed_frame frame;
        std::size_t consumed = 0;
        std::span<const std::byte> view(in_.data(), in_.size());
        decode_status st = try_decode_frame(view, cfg_.maxMessageBytes, frame, consumed);
        if (st == decode_status::NeedMore) break;
        if (st == decode_status::Error) {
            fail(error_category::Protocol, 0, "malformed/oversized frame");
            return;
        }

        lastRecv_ = event_loop::clock::now();

        switch (frame.type) {
        case frame_type::Hello:
        case frame_type::HelloAck: {
            if (state_ != conn_state::Handshaking) {
                // duplicate hello — ignore, but still consume
                break;
            }
            const hello& h = frame.hello_msg;
            if (h.protocolVersion != self_.protocolVersion || h.groupName != self_.groupName) {
                fail(error_category::Protocol, 0, "protocol/group mismatch");
                return;
            }
            peerId_         = h.nodeId;
            peerListenPort_ = h.tcpListenPort;
            peerNonce_      = h.nonce;
            state_          = conn_state::Established;
            in_.consume(consumed);
            listener_.on_peer_handshake(*this, h);
            if (dead_) return; // listener may have closed us (dial-race loser)
            continue;
        }
        case frame_type::Heartbeat:
            break; // lastRecv_ already updated
        case frame_type::Goodbye:
            in_.consume(consumed);
            fail(error_category::Socket, 0, "peer said goodbye");
            return;
        case frame_type::Data:
            if (state_ != conn_state::Established) {
                fail(error_category::Protocol, 0, "data before handshake");
                return;
            }
            // Deliver BEFORE consuming: frame.data.payload views the inbound
            // buffer, which consume() may compact/clear.
            listener_.on_peer_data(*this, frame.data);
            if (dead_) return;
            in_.consume(consumed);
            continue;
        case frame_type::Membership:
            if (state_ != conn_state::Established) {
                fail(error_category::Protocol, 0, "membership before handshake");
                return;
            }
            // frame.membership owns its data (decoded into a vector), so consume
            // order is safe.
            in_.consume(consumed);
            listener_.on_peer_membership(*this, frame.membership);
            if (dead_) return;
            continue;
        }
        in_.consume(consumed);
    }
}

void peer_connection::send_raw(std::span<const std::byte> frame) {
    if (dead_) return;
    if (out_.size() + frame.size() > cfg_.maxOutboundQueueBytes) {
        switch (cfg_.overflowPolicy) {
        case mesh_config::overflow::Disconnect:
            fail(error_category::Backpressure, 0, "outbound queue overflow");
            return;
        case mesh_config::overflow::DropNewest:
            listener_.report_error({error_category::Backpressure, 0,
                                   "dropped outbound (newest)", peerId_});
            return;
        case mesh_config::overflow::DropOldest:
            // Drop from the head until the new frame fits.
            while (!out_.empty() && out_.size() + frame.size() > cfg_.maxOutboundQueueBytes) {
                out_.consume(out_.size()); // coarse: clear backlog
            }
            break;
        }
    }
    out_.append(frame.data(), frame.size());
    lastSend_ = event_loop::clock::now();
    flush_outbound();
}

void peer_connection::flush_outbound() {
    while (!out_.empty()) {
        ssize_t n = ::send(sock_.get(), out_.data(), out_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out_.consume(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        fail(error_category::Socket, errno, "send failed");
        return;
    }
    update_epoll();
}

void peer_connection::update_epoll() {
    if (dead_ || !registered_) return;
    bool needWrite = !out_.empty() || state_ == conn_state::ConnectingOut;
    if (needWrite == wantWrite_) return;
    std::uint32_t ev = EPOLLIN | (needWrite ? EPOLLOUT : 0u);
    if (loop_.mod(sock_.get(), ev, this)) wantWrite_ = needWrite;
}

void peer_connection::maybe_heartbeat(event_loop::clock::time_point now,
                                    std::chrono::milliseconds interval) {
    if (state_ != conn_state::Established || dead_) return;
    if (now - lastSend_ >= interval) {
        auto hb = encode_heartbeat();
        send_raw(std::span<const std::byte>(hb.data(), hb.size()));
    }
}

void peer_connection::close_gracefully() {
    if (dead_) return;
    auto bye = encode_goodbye();
    out_.append(bye.data(), bye.size());
    flush_outbound();
    mark_dead();
}

void peer_connection::fail(error_category cat, int err, std::string what) {
    if (dead_) return;
    if (cat != error_category::Socket || err != 0) {
        listener_.report_error({cat, err, std::move(what),
                               peerId_.is_zero() ? std::nullopt : std::optional<peer_id>(peerId_)});
    }
    mark_dead();
}

void peer_connection::mark_dead() {
    if (dead_) return;
    dead_  = true;
    state_ = conn_state::Dead;
    if (registered_ && sock_.valid()) {
        loop_.del(sock_.get());
        registered_ = false;
    }
    listener_.on_peer_closed(*this); // owner schedules deferred reap
}

} // namespace mm
