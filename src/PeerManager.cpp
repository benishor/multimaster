#include "PeerManager.hpp"

#include <algorithm>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>

namespace mm {

PeerManager::PeerManager(EventLoop& loop, const MeshConfig& cfg, const LocalIdentity& self,
                         PeerManagerDelegate& delegate)
    : loop_(loop), cfg_(cfg), self_(self), delegate_(delegate),
      router_(cfg, loop, self.nodeId), rng_(std::random_device{}()) {}

PeerManager::~PeerManager() = default;

void PeerManager::start() {
    running_ = true;
    router_.start();
    dialSeeds();
    scheduleTick();
}

void PeerManager::shutdown() {
    running_ = false;
    for (auto& c : conns_) {
        if (c && c->state() == PeerConnection::State::Established) c->closeGracefully();
    }
    // Best-effort flush already done by closeGracefully(); drop everything.
    estab_.clear();
    conns_.clear();
    records_.clear();
    reap_.clear();
}

std::uint64_t PeerManager::randNonce() { return rng_(); }

// --- discovery & dialing ----------------------------------------------------

void PeerManager::onDiscovered(const Announce& a, const sockaddr_in& src) {
    auto& r = records_[a.nodeId];
    r.id = a.nodeId;
    r.lastSeen = EventLoop::Clock::now();

    // Dial-back target: source IP + advertised TCP listen port.
    sockaddr_in da = src;
    da.sin_port    = htons(a.tcpListenPort);
    r.dialAddr     = da;
    r.haveAddr     = true;

    if (!r.announced) {
        r.announced = true;
        delegate_.peerDiscovered(a.nodeId);
        emitKnownSnapshot();
    }
    if (r.state == PeerState::Lost) r.state = PeerState::Disconnected;

    maybeDial(r);
}

void PeerManager::maybeDial(PeerRecord& r) {
    if (!running_) return;
    if (r.id == self_.nodeId) return;
    if (r.state == PeerState::Connected || r.state == PeerState::Connecting) return;
    if (!r.haveAddr) return;
    // Fanout cap: 0 means dial everyone.
    if (cfg_.fanout != 0 && estab_.size() >= cfg_.fanout) return;
    startDial(r);
}

PeerConnection* PeerManager::startDial(PeerRecord& r) {
    auto conn = std::make_unique<PeerConnection>(loop_, *this, cfg_, self_,
                                                 r.dialAddr, randNonce());
    PeerConnection* p = conn.get();
    conns_.push_back(std::move(conn));
    r.state = PeerState::Connecting;
    r.conn  = p;
    if (!p->startConnect()) {
        // startConnect already marked it dead and called onPeerClosed (which set
        // the record back to Disconnected and scheduled a reconnect).
        return nullptr;
    }
    return p;
}

void PeerManager::startDialAddr(const sockaddr_in& addr) {
    auto conn = std::make_unique<PeerConnection>(loop_, *this, cfg_, self_,
                                                 addr, randNonce());
    PeerConnection* p = conn.get();
    conns_.push_back(std::move(conn));
    p->startConnect(); // peer id learned at handshake; no record yet
}

void PeerManager::acceptConnection(Socket sock, const sockaddr_in& peerAddr) {
    auto conn = std::make_unique<PeerConnection>(loop_, *this, cfg_, self_,
                                                 std::move(sock), peerAddr, randNonce());
    conns_.push_back(std::move(conn));
}

// --- connection events ------------------------------------------------------

bool PeerManager::keepExisting(PeerConnection* existing, PeerConnection* neu,
                               const PeerId& peer) const {
    // Same direction => duplicate; keep the older (existing).
    if (existing->inbound() == neu->inbound()) return true;
    // Different directions: keep the one initiated by the lower node id. A
    // connection is "initiated by us" iff it is outbound (!inbound).
    const bool weAreLower    = self_.nodeId < peer;
    const bool winnerInbound = !weAreLower; // if peer is lower, it initiated => inbound
    return existing->inbound() == winnerInbound;
}

void PeerManager::onPeerHandshake(PeerConnection& c, const Hello& peer) {
    const PeerId pid = c.peerId();
    if (pid == self_.nodeId) { // accidental self-connection
        c.closeGracefully();
        return;
    }

    auto& r = records_[pid];
    r.id = pid;

    // Learn/refresh dial-back address from the peer's advertised listen port.
    sockaddr_in da = c.peerAddr();
    da.sin_port    = htons(peer.tcpListenPort);
    r.dialAddr     = da;
    r.haveAddr     = true;
    r.lastSeen     = EventLoop::Clock::now();

    if (!r.announced) {
        r.announced = true;
        delegate_.peerDiscovered(pid);
        emitKnownSnapshot();
    }

    // Dial-race resolution.
    auto it = estab_.find(pid);
    PeerConnection* existing = (it != estab_.end() && it->second != &c) ? it->second : nullptr;
    if (existing && keepExisting(existing, &c, pid)) {
        c.closeGracefully(); // the new connection loses
        return;
    }

    // The new connection wins (or there was none). Install it first so that
    // closing the loser doesn't look like a disconnect for this peer.
    const bool wasConnected = (r.state == PeerState::Connected);
    estab_[pid]             = &c;
    r.conn                  = &c;
    r.state                 = PeerState::Connected;
    r.reconnectAttempts     = 0;

    if (existing) existing->closeGracefully();

    if (!wasConnected) {
        delegate_.peerConnected(pid);
        emitConnectedSnapshot();
    }
}

void PeerManager::onPeerData(PeerConnection& c, const DataView& view) {
    router_.onData(view, c.fd(), *this);
}

void PeerManager::onPeerClosed(PeerConnection& c) {
    const PeerId pid = c.peerId();

    // Detach from the established map if this was the live link for its peer.
    if (!pid.isZero()) {
        auto e = estab_.find(pid);
        if (e != estab_.end() && e->second == &c) estab_.erase(e);
    }

    // Find the owning record (by id, else by pointer for pre-handshake dials).
    PeerRecord* rec = nullptr;
    if (!pid.isZero()) {
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
        const bool wasConnected = (rec->state == PeerState::Connected);
        rec->state              = PeerState::Disconnected;
        if (wasConnected) {
            delegate_.peerDisconnected(rec->id);
            emitConnectedSnapshot();
        }
        scheduleReconnect(*rec);
    }

    reap_.push_back(&c);
    scheduleReap();
}

void PeerManager::reportError(const Error& e) { delegate_.error(e); }

// --- forwarding (Forwarder) -------------------------------------------------

void PeerManager::forwardExcept(std::span<const std::byte> frame, int exceptFd) {
    // Snapshot targets first: sendRaw may close (and detach) a connection,
    // which would otherwise invalidate the estab_ iterator mid-loop.
    std::vector<PeerConnection*> targets;
    targets.reserve(estab_.size());
    for (auto& [id, c] : estab_) {
        if (c->fd() != exceptFd) targets.push_back(c);
    }
    for (auto* c : targets) c->sendRaw(frame);
}

bool PeerManager::forwardTo(const PeerId& dst, std::span<const std::byte> frame) {
    auto it = estab_.find(dst);
    if (it == estab_.end()) return false;
    it->second->sendRaw(frame);
    return true;
}

void PeerManager::deliverLocal(const PeerId& from, Bytes payload) {
    delegate_.messageReceived(from, payload);
}

// --- reconnect / liveness ---------------------------------------------------

void PeerManager::scheduleReconnect(PeerRecord& r) {
    if (!running_) return;
    if (r.state == PeerState::Connected || r.state == PeerState::Lost) return;

    // Exponential backoff with jitter, capped at reconnectMax.
    long long base = cfg_.reconnectBase.count();
    int       att  = std::min(r.reconnectAttempts, 20);
    long long ms   = base << att; // base * 2^attempts
    long long cap  = cfg_.reconnectMax.count();
    if (ms > cap || ms < 0) ms = cap;
    std::uniform_int_distribution<long long> jit(0, std::max<long long>(1, ms / 5));
    ms += jit(rng_);
    r.reconnectAttempts++;

    PeerId id = r.id;
    loop_.addTimer(std::chrono::milliseconds(ms), [this, id] { onReconnectTimer(id); });
}

void PeerManager::onReconnectTimer(PeerId id) {
    if (!running_) return;
    auto it = records_.find(id);
    if (it == records_.end()) return;
    auto& r = it->second;
    if (r.state == PeerState::Connected || r.state == PeerState::Connecting) return;
    if (r.state == PeerState::Lost) return;
    // Only retry while the peer still appears to be on the LAN.
    if (EventLoop::Clock::now() - r.lastSeen > cfg_.peerLostTimeout) return;
    maybeDial(r);
}

void PeerManager::tick() {
    if (!running_) return;
    const auto now = EventLoop::Clock::now();

    // Heartbeats + liveness timeouts. closeGracefully() => onPeerClosed (which
    // only marks for deferred reaping), so conns_ is not mutated here.
    for (auto& c : conns_) {
        if (!c) continue;
        switch (c->state()) {
        case PeerConnection::State::Established:
            c->maybeHeartbeat(now, cfg_.heartbeatInterval);
            if (now - c->lastRecv() > cfg_.heartbeatTimeout) c->closeGracefully();
            break;
        case PeerConnection::State::Handshaking:
        case PeerConnection::State::ConnectingOut:
            if (now - c->createdAt() > cfg_.handshakeTimeout) c->closeGracefully();
            break;
        case PeerConnection::State::Dead:
            break;
        }
    }

    // Lost detection: peers we haven't heard announce from in a while and are
    // not currently connected.
    std::vector<PeerId> toLose;
    for (auto& [id, r] : records_) {
        if (r.state == PeerState::Connected || r.state == PeerState::Lost) continue;
        if (now - r.lastSeen > cfg_.peerLostTimeout) toLose.push_back(id);
    }
    if (!toLose.empty()) {
        for (const auto& id : toLose) {
            delegate_.peerLost(id);
            records_.erase(id);
        }
        emitKnownSnapshot();
    }

    // Fallback: if isolated, re-dial seed peers.
    if (estab_.empty()) dialSeeds();

    scheduleTick();
}

void PeerManager::scheduleTick() {
    loop_.addTimer(cfg_.heartbeatInterval, [this] { tick(); });
}

void PeerManager::dialSeeds() {
    if (!running_) return;
    for (const auto& s : cfg_.seedPeers) {
        sockaddr_in addr{};
        if (makeAddr(s.host, s.port, addr)) {
            startDialAddr(addr);
            continue;
        }
        // Resolve a hostname (blocking, but seeds are few and only at startup /
        // while isolated).
        addrinfo  hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(s.host.c_str(), nullptr, &hints, &res) == 0 && res) {
            auto* in = reinterpret_cast<sockaddr_in*>(res->ai_addr);
            in->sin_port = htons(s.port);
            startDialAddr(*in);
            ::freeaddrinfo(res);
        }
    }
}

// --- deferred reaping -------------------------------------------------------

void PeerManager::scheduleReap() {
    if (reapScheduled_) return;
    reapScheduled_ = true;
    loop_.addTimer(std::chrono::milliseconds(0), [this] { reap(); });
}

void PeerManager::reap() {
    reapScheduled_ = false;
    if (reap_.empty()) return;

    // Remove the dead connections from conns_ (their unique_ptr owns lifetime).
    std::vector<PeerConnection*> dead;
    dead.swap(reap_);
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [&](const std::unique_ptr<PeerConnection>& up) {
                                    return std::find(dead.begin(), dead.end(), up.get()) != dead.end();
                                }),
                 conns_.end());
}

// --- snapshots --------------------------------------------------------------

void PeerManager::emitConnectedSnapshot() {
    std::vector<PeerId> v;
    v.reserve(estab_.size());
    for (auto& [id, c] : estab_) v.push_back(id);
    delegate_.connectedSnapshot(std::move(v));
}

void PeerManager::emitKnownSnapshot() {
    std::vector<PeerId> v;
    v.reserve(records_.size());
    for (auto& [id, r] : records_) v.push_back(id);
    delegate_.knownSnapshot(std::move(v));
}

} // namespace mm
