#include "peer_manager.hpp"

#include <algorithm>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>

namespace mm {

peer_manager::peer_manager(event_loop& loop, const mesh_config& cfg, const local_identity& self,
                         peer_manager_delegate& delegate)
    : loop_(loop), cfg_(cfg), self_(self), delegate_(delegate),
      router_(cfg, loop, self.nodeId), rng_(std::random_device{}()) {}

peer_manager::~peer_manager() = default;

void peer_manager::start() {
    running_ = true;
    router_.start();

    // Register configured static (persistent) peers.
    for (const auto& s : cfg_.staticPeers) {
        static_targets_.push_back(static_target{s.host, s.port, std::nullopt, 0, {}, nullptr});
    }

    dial_seeds();
    maintain_static_peers();
    schedule_tick();
}

void peer_manager::shutdown() {
    running_ = false;
    for (auto& c : conns_) {
        if (c && c->state() == peer_connection::conn_state::Established) c->close_gracefully();
    }
    // Best-effort flush already done by close_gracefully(); drop everything.
    estab_.clear();
    conns_.clear();
    records_.clear();
    static_targets_.clear();
    static_dial_conn_.clear();
    reap_.clear();
}

std::uint64_t peer_manager::rand_nonce() { return rng_(); }

// --- discovery & dialing ----------------------------------------------------

void peer_manager::on_discovered(const announce& a, const sockaddr_in& src) {
    auto& r = records_[a.nodeId];
    r.id = a.nodeId;
    r.lastSeen = event_loop::clock::now();

    // Dial-back target: source IP + advertised TCP listen port.
    sockaddr_in da = src;
    da.sin_port    = htons(a.tcpListenPort);
    r.dialAddr     = da;
    r.haveAddr     = true;

    if (!r.announced) {
        r.announced = true;
        delegate_.peer_discovered(a.nodeId);
        emit_known_snapshot();
    }
    if (r.state == peer_state::Lost) r.state = peer_state::Disconnected;

    maybe_dial(r);
}

void peer_manager::maybe_dial(peer_record& r) {
    if (!running_) return;
    if (r.id == self_.nodeId) return;
    if (r.state == peer_state::Connected || r.state == peer_state::Connecting) return;
    if (!r.haveAddr) return;
    // Fanout cap: 0 means dial everyone.
    if (cfg_.fanout != 0 && estab_.size() >= cfg_.fanout) return;
    start_dial(r);
}

peer_connection* peer_manager::start_dial(peer_record& r) {
    auto conn = std::make_unique<peer_connection>(loop_, *this, cfg_, self_,
                                                 r.dialAddr, rand_nonce());
    peer_connection* p = conn.get();
    conns_.push_back(std::move(conn));
    r.state = peer_state::Connecting;
    r.conn  = p;
    if (!p->start_connect()) {
        // startConnect already marked it dead and called on_peer_closed (which set
        // the record back to Disconnected and scheduled a reconnect).
        return nullptr;
    }
    return p;
}

void peer_manager::start_dial_addr(const sockaddr_in& addr) {
    auto conn = std::make_unique<peer_connection>(loop_, *this, cfg_, self_,
                                                 addr, rand_nonce());
    peer_connection* p = conn.get();
    conns_.push_back(std::move(conn));
    p->start_connect(); // peer id learned at handshake; no record yet
}

void peer_manager::accept_connection(socket sock, const sockaddr_in& peerAddr) {
    auto conn = std::make_unique<peer_connection>(loop_, *this, cfg_, self_,
                                                 std::move(sock), peerAddr, rand_nonce());
    conns_.push_back(std::move(conn));
}

// --- connection events ------------------------------------------------------

bool peer_manager::keep_existing(peer_connection* existing, peer_connection* neu,
                               const peer_id& peer) const {
    // Same direction => duplicate; keep the older (existing).
    if (existing->inbound() == neu->inbound()) return true;
    // Different directions: keep the one initiated by the lower node id. A
    // connection is "initiated by us" iff it is outbound (!inbound).
    const bool weAreLower    = self_.nodeId < peer;
    const bool winnerInbound = !weAreLower; // if peer is lower, it initiated => inbound
    return existing->inbound() == winnerInbound;
}

void peer_manager::on_peer_handshake(peer_connection& c, const hello& peer) {
    const peer_id pid = c.id();
    if (pid == self_.nodeId) { // accidental self-connection
        c.close_gracefully();
        return;
    }

    // If this connection originated from a static-peer dial, bind the target to
    // the learned id and flag the record so it is maintained persistently and
    // never pruned as "lost". Done before dial-race resolution so the binding
    // holds even if this particular connection loses the race.
    bool fromStatic = false;
    if (auto sd = static_dial_conn_.find(&c); sd != static_dial_conn_.end()) {
        auto& t = static_targets_[sd->second];
        if (t.learnedId && !(*t.learnedId == pid)) {
            // The endpoint now answers with a different id (peer restarted /
            // repointed). Drop the stale record so it doesn't linger.
            records_.erase(*t.learnedId);
            emit_known_snapshot();
        }
        t.learnedId = pid;
        t.attempts  = 0;
        t.dialing   = nullptr;
        static_dial_conn_.erase(sd);
        fromStatic = true;
    }

    auto& r = records_[pid];
    r.id = pid;
    if (fromStatic) r.isStatic = true;

    // Learn/refresh dial-back address from the peer's advertised listen port.
    sockaddr_in da = c.peer_addr();
    da.sin_port    = htons(peer.tcpListenPort);
    r.dialAddr     = da;
    r.haveAddr     = true;
    r.lastSeen     = event_loop::clock::now();

    if (!r.announced) {
        r.announced = true;
        delegate_.peer_discovered(pid);
        emit_known_snapshot();
    }

    // Dial-race resolution.
    auto it = estab_.find(pid);
    peer_connection* existing = (it != estab_.end() && it->second != &c) ? it->second : nullptr;
    if (existing && keep_existing(existing, &c, pid)) {
        c.close_gracefully(); // the new connection loses
        return;
    }

    // The new connection wins (or there was none). Install it first so that
    // closing the loser doesn't look like a disconnect for this peer.
    const bool wasConnected = (r.state == peer_state::Connected);
    estab_[pid]             = &c;
    r.conn                  = &c;
    r.state                 = peer_state::Connected;
    r.reconnectAttempts     = 0;

    if (existing) existing->close_gracefully();

    if (!wasConnected) {
        delegate_.peer_connected(pid);
        emit_connected_snapshot();
    }
}

void peer_manager::on_peer_data(peer_connection& c, const data_view& view) {
    router_.on_data(view, c.fd(), *this);
}

void peer_manager::on_peer_closed(peer_connection& c) {
    const peer_id pid = c.id();

    // If this was an in-flight static dial, free the slot so maintenance can
    // redial the endpoint (re-resolving DNS) after the backoff interval.
    if (auto sd = static_dial_conn_.find(&c); sd != static_dial_conn_.end()) {
        static_targets_[sd->second].dialing = nullptr;
        static_dial_conn_.erase(sd);
    }

    // Detach from the established map if this was the live link for its peer.
    if (!pid.is_zero()) {
        auto e = estab_.find(pid);
        if (e != estab_.end() && e->second == &c) estab_.erase(e);
    }

    // Find the owning record (by id, else by pointer for pre-handshake dials).
    peer_record* rec = nullptr;
    if (!pid.is_zero()) {
        auto it = records_.find(pid);
        if (it != records_.end()) rec = &it->second;
    }
    if (!rec) {
        for (auto& [id, r] : records_) {
            if (r.conn == &c) { rec = &r; break; }
        }
    }

    if (rec && rec->conn == &c) {
        rec->conn               = nullptr;
        const bool wasConnected = (rec->state == peer_state::Connected);
        rec->state              = peer_state::Disconnected;
        if (wasConnected) {
            delegate_.peer_disconnected(rec->id);
            emit_connected_snapshot();
        }
        // Static peers are redialed by maintain_static_peers() (by endpoint, with
        // DNS re-resolution); all others use the id-keyed reconnect path.
        if (rec->isStatic) {
            maintain_static_peers();
        } else {
            schedule_reconnect(*rec);
        }
    }

    reap_.push_back(&c);
    schedule_reap();
}

void peer_manager::report_error(const error& e) { delegate_.on_error(e); }

// --- forwarding (forwarder) -------------------------------------------------

void peer_manager::forward_except(std::span<const std::byte> frame, int exceptFd) {
    // Snapshot targets first: sendRaw may close (and detach) a connection,
    // which would otherwise invalidate the estab_ iterator mid-loop.
    std::vector<peer_connection*> targets;
    targets.reserve(estab_.size());
    for (auto& [id, c] : estab_) {
        if (c->fd() != exceptFd) targets.push_back(c);
    }
    for (auto* c : targets) c->send_raw(frame);
}

bool peer_manager::forward_to(const peer_id& dst, std::span<const std::byte> frame) {
    auto it = estab_.find(dst);
    if (it == estab_.end()) return false;
    it->second->send_raw(frame);
    return true;
}

void peer_manager::deliver_local(const peer_id& from, bytes payload) {
    delegate_.message_received(from, payload);
}

// --- reconnect / liveness ---------------------------------------------------

void peer_manager::schedule_reconnect(peer_record& r) {
    if (!running_) return;
    if (r.state == peer_state::Connected || r.state == peer_state::Lost) return;

    // Exponential backoff with jitter, capped at reconnectMax.
    long long base = cfg_.reconnectBase.count();
    int       att  = std::min(r.reconnectAttempts, 20);
    long long ms   = base << att; // base * 2^attempts
    long long cap  = cfg_.reconnectMax.count();
    if (ms > cap || ms < 0) ms = cap;
    std::uniform_int_distribution<long long> jit(0, std::max<long long>(1, ms / 5));
    ms += jit(rng_);
    r.reconnectAttempts++;

    peer_id id = r.id;
    loop_.add_timer(std::chrono::milliseconds(ms), [this, id] { on_reconnect_timer(id); });
}

void peer_manager::on_reconnect_timer(peer_id id) {
    if (!running_) return;
    auto it = records_.find(id);
    if (it == records_.end()) return;
    auto& r = it->second;
    if (r.state == peer_state::Connected || r.state == peer_state::Connecting) return;
    if (r.state == peer_state::Lost) return;
    // Only retry while the peer still appears to be on the LAN.
    if (event_loop::clock::now() - r.lastSeen > cfg_.peerLostTimeout) return;
    maybe_dial(r);
}

void peer_manager::tick() {
    if (!running_) return;
    const auto now = event_loop::clock::now();

    // Heartbeats + liveness timeouts. close_gracefully() => on_peer_closed (which
    // only marks for deferred reaping), so conns_ is not mutated here.
    for (auto& c : conns_) {
        if (!c) continue;
        switch (c->state()) {
        case peer_connection::conn_state::Established:
            c->maybe_heartbeat(now, cfg_.heartbeatInterval);
            if (now - c->last_recv() > cfg_.heartbeatTimeout) c->close_gracefully();
            break;
        case peer_connection::conn_state::Handshaking:
        case peer_connection::conn_state::ConnectingOut:
            if (now - c->created_at() > cfg_.handshakeTimeout) c->close_gracefully();
            break;
        case peer_connection::conn_state::Dead:
            break;
        }
    }

    // Lost detection: peers we haven't heard announce from in a while and are
    // not currently connected. Static peers are exempt — they are persistent by
    // contract and may be silent (no multicast) for long stretches.
    std::vector<peer_id> toLose;
    for (auto& [id, r] : records_) {
        if (r.isStatic) continue;
        if (r.state == peer_state::Connected || r.state == peer_state::Lost) continue;
        if (now - r.lastSeen > cfg_.peerLostTimeout) toLose.push_back(id);
    }
    if (!toLose.empty()) {
        for (const auto& id : toLose) {
            delegate_.peer_lost(id);
            records_.erase(id);
        }
        emit_known_snapshot();
    }

    // Keep persistent static-peer connections up.
    maintain_static_peers();

    // Fallback: if isolated, re-dial seed peers.
    if (estab_.empty()) dial_seeds();

    schedule_tick();
}

void peer_manager::schedule_tick() {
    loop_.add_timer(cfg_.heartbeatInterval, [this] { tick(); });
}

bool peer_manager::resolve_endpoint(const std::string& host, std::uint16_t port,
                                    sockaddr_in& out) const {
    // Fast path: a numeric IPv4 literal needs no DNS.
    if (make_addr(host, port, out)) return true;
    // Otherwise resolve the hostname. Blocking, but endpoints are few and this
    // happens only at startup / on reconnect intervals. Fresh resolution each
    // call lets dynamic-DNS / rolling-IP internet peers keep working.
    addrinfo  hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out.sin_port = htons(port);
    ::freeaddrinfo(res);
    return true;
}

void peer_manager::dial_seeds() {
    if (!running_) return;
    for (const auto& s : cfg_.seedPeers) {
        sockaddr_in addr{};
        if (resolve_endpoint(s.host, s.port, addr)) start_dial_addr(addr);
    }
}

// --- static (persistent) peers ---------------------------------------------

void peer_manager::maintain_static_peers() {
    if (!running_) return;
    const auto now = event_loop::clock::now();

    for (std::size_t i = 0; i < static_targets_.size(); ++i) {
        auto& t = static_targets_[i];

        // Already connected to this peer (via this dial or any other link)?
        if (t.learnedId && estab_.count(*t.learnedId)) {
            t.attempts = 0;
            continue;
        }
        if (t.dialing) continue;       // a dial is in flight
        if (now < t.nextAttempt) continue; // still backing off

        // Exponential backoff with jitter for the *next* attempt.
        long long base = cfg_.reconnectBase.count();
        int       att  = std::min(t.attempts, 20);
        long long ms   = base << att;
        long long cap  = cfg_.reconnectMax.count();
        if (ms > cap || ms < 0) ms = cap;
        std::uniform_int_distribution<long long> jit(0, std::max<long long>(1, ms / 5));
        t.nextAttempt = now + std::chrono::milliseconds(ms + jit(rng_));
        t.attempts++;

        start_static_dial(i);
    }
}

void peer_manager::start_static_dial(std::size_t targetIdx) {
    auto& t = static_targets_[targetIdx];
    sockaddr_in addr{};
    if (!resolve_endpoint(t.host, t.port, addr)) {
        delegate_.on_error({error_category::Discovery, 0,
                            "static peer DNS resolution failed: " + t.host, std::nullopt});
        return; // backoff already armed; retry on a later tick
    }
    auto conn = std::make_unique<peer_connection>(loop_, *this, cfg_, self_,
                                                  addr, rand_nonce());
    peer_connection* p = conn.get();
    conns_.push_back(std::move(conn));
    t.dialing                = p;
    static_dial_conn_[p]     = targetIdx;
    p->start_connect(); // peer id learned at handshake; record created there
}

// --- deferred reaping -------------------------------------------------------

void peer_manager::schedule_reap() {
    if (reapScheduled_) return;
    reapScheduled_ = true;
    loop_.add_timer(std::chrono::milliseconds(0), [this] { reap(); });
}

void peer_manager::reap() {
    reapScheduled_ = false;
    if (reap_.empty()) return;

    // Remove the dead connections from conns_ (their unique_ptr owns lifetime).
    std::vector<peer_connection*> dead;
    dead.swap(reap_);
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [&](const std::unique_ptr<peer_connection>& up) {
                                    return std::find(dead.begin(), dead.end(), up.get()) != dead.end();
                                }),
                 conns_.end());
}

// --- snapshots --------------------------------------------------------------

void peer_manager::emit_connected_snapshot() {
    std::vector<peer_id> v;
    v.reserve(estab_.size());
    for (auto& [id, c] : estab_) v.push_back(id);
    delegate_.connected_snapshot(std::move(v));
}

void peer_manager::emit_known_snapshot() {
    std::vector<peer_id> v;
    v.reserve(records_.size());
    for (auto& [id, r] : records_) v.push_back(id);
    delegate_.known_snapshot(std::move(v));
}

} // namespace mm
