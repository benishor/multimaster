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
      state_(conn_state::Handshaking), localNonce_(nonce), secure_(self.secure),
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
      localNonce_(nonce), secure_(self.secure), createdAt_(event_loop::clock::now()),
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
    h.secure          = secure_;
    if (secure_) {
        ephKeys_     = crypto::gen_ephemeral(); // fresh per connection => forward secrecy
        h.ephPubKey  = ephKeys_.pk;
    }
    if (self_.hasIdentity) {
        h.hasIdentity = true;
        h.idPubKey    = self_.idKeys.pk;
        h.nodeName    = self_.nodeName;
    }
    auto frame = encode_hello(inbound_ ? frame_type::HelloAck : frame_type::Hello, h);
    out_.append(frame.data(), frame.size());
    lastSend_ = event_loop::clock::now();
    flush_outbound();
}

void peer_connection::send_auth_confirm() {
    auto frame = encode_auth_confirm(myConfirmTag_, useIdentity_, myIdSig_);
    out_.append(frame.data(), frame.size()); // confirmation is sent in the clear
    lastSend_ = event_loop::clock::now();
    flush_outbound();
}

std::vector<std::byte> peer_connection::id_transcript(const std::array<std::byte, 32>& firstEph,
                                                     const std::array<std::byte, 32>& secondEph,
                                                     const std::string& name) const {
    static constexpr char label[] = "mm-id-v1";
    std::vector<std::byte> m;
    m.reserve((sizeof(label) - 1) + firstEph.size() + secondEph.size() + name.size());
    m.insert(m.end(), reinterpret_cast<const std::byte*>(label),
             reinterpret_cast<const std::byte*>(label) + (sizeof(label) - 1));
    m.insert(m.end(), firstEph.begin(), firstEph.end());
    m.insert(m.end(), secondEph.begin(), secondEph.end());
    m.insert(m.end(), reinterpret_cast<const std::byte*>(name.data()),
             reinterpret_cast<const std::byte*>(name.data()) + name.size());
    return m;
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
    // Handshake frames (Hello / AuthConfirm) are always plaintext. In a secured
    // mesh, once Established the remaining stream is encrypted records, so we stop
    // reading plaintext from in_ and switch to the decrypt path.
    if (!(secure_ && state_ == conn_state::Established)) {
        if (!parse_plaintext()) return;
    }
    if (secure_ && state_ == conn_state::Established) {
        if (!drain_records()) return;
        parse_secure();
    }
}

bool peer_connection::parse_plaintext() {
    for (;;) {
        parsed_frame frame;
        std::size_t  consumed = 0;
        std::span<const std::byte> view(in_.data(), in_.size());
        decode_status st = try_decode_frame(view, cfg_.maxMessageBytes, frame, consumed);
        if (st == decode_status::NeedMore) break;
        if (st == decode_status::Error) {
            fail(error_category::Protocol, 0, "malformed/oversized frame");
            return false;
        }
        if (!dispatch(frame, consumed, in_)) return false;
        // Secured handshake just completed: the rest of in_ is encrypted records.
        if (secure_ && state_ == conn_state::Established) break;
    }
    return true;
}

bool peer_connection::drain_records() {
    for (;;) {
        if (in_.size() < 4) break;
        const std::byte* p    = in_.data();
        std::uint32_t    clen = (static_cast<std::uint32_t>(p[0]) << 24) |
                             (static_cast<std::uint32_t>(p[1]) << 16) |
                             (static_cast<std::uint32_t>(p[2]) << 8) |
                             (static_cast<std::uint32_t>(p[3]));
        if (clen < 16 || clen > cfg_.maxMessageBytes + 64) {
            fail(error_category::Protocol, 0, "malformed encrypted record");
            return false;
        }
        if (in_.size() < 4 + static_cast<std::size_t>(clen)) break; // need more
        std::span<const std::byte> ct(p + 4, clen);
        std::vector<std::byte>     pt;
        if (!sess_->open(ct, pt)) {
            fail(error_category::Crypto, 0, "record authentication failed");
            return false;
        }
        plain_.append(pt.data(), pt.size());
        in_.consume(4 + clen);
    }
    return true;
}

void peer_connection::parse_secure() {
    for (;;) {
        parsed_frame frame;
        std::size_t  consumed = 0;
        std::span<const std::byte> view(plain_.data(), plain_.size());
        decode_status st = try_decode_frame(view, cfg_.maxMessageBytes, frame, consumed);
        if (st == decode_status::NeedMore) break;
        if (st == decode_status::Error) {
            fail(error_category::Protocol, 0, "malformed decrypted frame");
            return;
        }
        if (!dispatch(frame, consumed, plain_)) return;
    }
}

bool peer_connection::dispatch(parsed_frame& frame, std::size_t consumed, buffer& src) {
    lastRecv_ = event_loop::clock::now();

    switch (frame.type) {
    case frame_type::Hello:
    case frame_type::HelloAck:
        return handle_hello(frame, consumed, src);
    case frame_type::AuthConfirm:
        return handle_auth_confirm(frame, consumed, src);
    case frame_type::Heartbeat:
        src.consume(consumed); // lastRecv_ already updated
        return true;
    case frame_type::Goodbye:
        src.consume(consumed);
        fail(error_category::Socket, 0, "peer said goodbye");
        return false;
    case frame_type::Data:
        if (state_ != conn_state::Established) {
            fail(error_category::Protocol, 0, "data before handshake");
            return false;
        }
        // Deliver BEFORE consuming: frame.data.payload views `src`, which
        // consume() may compact/clear.
        listener_.on_peer_data(*this, frame.data);
        if (dead_) return false;
        src.consume(consumed);
        return true;
    case frame_type::Membership:
        if (state_ != conn_state::Established) {
            fail(error_category::Protocol, 0, "membership before handshake");
            return false;
        }
        // frame.membership owns its data (decoded into a vector), so consume
        // order is safe.
        src.consume(consumed);
        listener_.on_peer_membership(*this, frame.membership);
        if (dead_) return false;
        return true;
    case frame_type::IdentityRecord:
        if (state_ != conn_state::Established) {
            fail(error_category::Protocol, 0, "identity record before handshake");
            return false;
        }
        src.consume(consumed); // frame.identity owns its data (copied out)
        listener_.on_peer_identity(*this, frame.identity);
        if (dead_) return false;
        return true;
    }
    src.consume(consumed);
    return true;
}

bool peer_connection::handle_hello(parsed_frame& frame, std::size_t consumed, buffer& src) {
    if (state_ != conn_state::Handshaking) {
        src.consume(consumed); // duplicate hello — ignore
        return true;
    }
    const hello& h = frame.hello_msg;
    if (h.protocolVersion != self_.protocolVersion || h.groupName != self_.groupName) {
        fail(error_category::Protocol, 0, "protocol/group mismatch");
        return false;
    }
    if (h.secure != secure_) {
        fail(error_category::Protocol, 0, "security mode mismatch");
        return false;
    }
    peerId_         = h.nodeId;
    peerListenPort_ = h.tcpListenPort;
    peerNonce_      = h.nonce;
    src.consume(consumed);

    if (!secure_) {
        state_ = conn_state::Established;
        listener_.on_peer_handshake(*this, h);
        return !dead_; // listener may have closed us (dial-race loser)
    }

    // If we run an identity and require peers to as well, a peer without one is
    // rejected up front.
    if (self_.hasIdentity && cfg_.requireIdentity && !h.hasIdentity) {
        fail(error_category::Identity, 0, "peer presented no identity");
        return false;
    }

    // Secured: derive the session from the peer's ephemeral key, then prove
    // possession of the PSK with a confirmation tag. The peer is not installed
    // until its own confirmation verifies.
    crypto::handshake_result hr;
    if (!crypto::do_handshake(ephKeys_, h.ephPubKey, self_.groupKey, self_.groupName, hr)) {
        fail(error_category::Crypto, 0, "key agreement failed");
        return false;
    }
    sess_            = std::move(hr.session);
    myConfirmTag_    = hr.myConfirmTag;
    expectedPeerTag_ = hr.expectedPeerTag;
    peerHello_       = h;

    // Identity runs only when both ends present one; sign the transcript binding
    // our static key to this connection's ephemeral keys and our name.
    useIdentity_ = self_.hasIdentity && h.hasIdentity;
    if (useIdentity_) {
        auto msg = id_transcript(ephKeys_.pk, h.ephPubKey, self_.nodeName);
        myIdSig_ = crypto::sign(std::span<const std::byte>(msg.data(), msg.size()),
                                self_.idKeys.sk);
    }

    state_ = conn_state::AwaitingConfirm;
    send_auth_confirm();
    return !dead_;
}

bool peer_connection::handle_auth_confirm(parsed_frame& frame, std::size_t consumed, buffer& src) {
    if (!secure_ || state_ != conn_state::AwaitingConfirm) {
        fail(error_category::Protocol, 0, "unexpected auth confirm");
        return false;
    }
    src.consume(consumed);
    if (!crypto::verify_tag(frame.auth_tag, expectedPeerTag_)) {
        fail(error_category::Crypto, 0, "authentication failed");
        return false;
    }

    if (useIdentity_) {
        if (!frame.has_auth_sig) {
            fail(error_category::Identity, 0, "missing identity signature");
            return false;
        }
        // Self-certifying: the claimed nodeId must equal hash(identity pubkey).
        auto derived = crypto::id_from_identity(peerHello_.idPubKey);
        if (std::memcmp(derived.data(), peerId_.bytes.data(), derived.size()) != 0) {
            fail(error_category::Identity, 0, "node id does not match identity key");
            return false;
        }
        // The signature proves the peer holds the private key and authenticates
        // its name, bound to this connection's ephemeral keys.
        auto msg = id_transcript(peerHello_.ephPubKey, ephKeys_.pk, peerHello_.nodeName);
        if (!crypto::verify_sig(std::span<const std::byte>(msg.data(), msg.size()), frame.auth_sig,
                                peerHello_.idPubKey)) {
            fail(error_category::Identity, 0, "identity signature invalid");
            return false;
        }
        // Allowlist: per-node admit/revoke without rotating the PSK.
        if (!self_.trustedKeys.empty()) {
            bool trusted = false;
            for (const auto& k : self_.trustedKeys)
                if (k == peerHello_.idPubKey) { trusted = true; break; }
            if (!trusted) {
                fail(error_category::Identity, 0, "peer identity not in allowlist");
                return false;
            }
        }
    }

    state_ = conn_state::Established;
    listener_.on_peer_handshake(*this, peerHello_);
    return !dead_; // listener may have closed us (dial-race loser)
}

void peer_connection::enqueue(std::span<const std::byte> frame) {
    if (dead_) return;
    std::vector<std::byte>     sealed;
    std::span<const std::byte> rec = frame;
    if (secure_ && state_ == conn_state::Established) {
        sealed = sess_->seal(frame);
        rec    = std::span<const std::byte>(sealed.data(), sealed.size());
    }
    if (out_.size() + rec.size() > cfg_.maxOutboundQueueBytes) {
        switch (cfg_.overflowPolicy) {
        case mesh_config::overflow::Disconnect:
            fail(error_category::Backpressure, 0, "outbound queue overflow");
            return;
        case mesh_config::overflow::DropNewest:
            listener_.report_error({error_category::Backpressure, 0,
                                   "dropped outbound (newest)", peerId_});
            return;
        case mesh_config::overflow::DropOldest:
            // Drop from the head until the new record fits.
            while (!out_.empty() && out_.size() + rec.size() > cfg_.maxOutboundQueueBytes) {
                out_.consume(out_.size()); // coarse: clear backlog
            }
            break;
        }
    }
    out_.append(rec.data(), rec.size());
    lastSend_ = event_loop::clock::now();
    flush_outbound();
}

void peer_connection::send_raw(std::span<const std::byte> frame) { enqueue(frame); }

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
    enqueue(std::span<const std::byte>(bye.data(), bye.size())); // sealed if secured; flushes
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
