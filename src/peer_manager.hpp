#pragma once

#include "event_loop.hpp"
#include "gossip_router.hpp"
#include "peer_connection.hpp"
#include "socket.hpp"
#include "wire.hpp"
#include "multimaster/config.hpp"
#include "multimaster/events.hpp"
#include "multimaster/peer_id.hpp"

#include <cstdint>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include <netinet/in.h>

namespace mm {

/// Sink for mesh-level events. Implemented by MeshImpl, which forwards to the
/// user's Callbacks and maintains thread-safe state snapshots.
class PeerManagerDelegate {
public:
    virtual ~PeerManagerDelegate() = default;
    virtual void peerDiscovered(const PeerId&)                      = 0;
    virtual void peerConnected(const PeerId&)                       = 0;
    virtual void peerDisconnected(const PeerId&)                    = 0;
    virtual void peerLost(const PeerId&)                            = 0;
    virtual void messageReceived(const PeerId& from, Bytes payload) = 0;
    virtual void error(const Error&)                                = 0;
    /// New snapshots whenever the relevant set changes (IO thread).
    virtual void connectedSnapshot(std::vector<PeerId>)             = 0;
    virtual void knownSnapshot(std::vector<PeerId>)                 = 0;
};

/// Owns all peer connections and the mesh's logical state: discovery → dial →
/// handshake → dial-race resolution → liveness → reconnect/backoff → lost.
/// Acts as the ConnectionListener for every PeerConnection and as the Forwarder
/// for the GossipRouter.
class PeerManager : public ConnectionListener, public Forwarder {
public:
    PeerManager(EventLoop& loop, const MeshConfig& cfg, const LocalIdentity& self,
                PeerManagerDelegate& delegate);
    ~PeerManager() override;

    void start();
    void shutdown(); // send Goodbye to all and tear down

    // From Discovery.
    void onDiscovered(const Announce& a, const sockaddr_in& src);
    // From Listener.
    void acceptConnection(Socket sock, const sockaddr_in& peerAddr);

    // Origination from the public API (runs on IO thread).
    void originateBroadcast(Bytes payload) { router_.originateBroadcast(payload, *this); }
    void originateTargeted(const PeerId& dst, Bytes payload) {
        router_.originateTargeted(dst, payload, *this);
    }

    // ConnectionListener
    void onPeerHandshake(PeerConnection& c, const Hello& peer) override;
    void onPeerData(PeerConnection& c, const DataView& view) override;
    void onPeerClosed(PeerConnection& c) override;
    void reportError(const Error& e) override;

    // Forwarder
    void forwardExcept(std::span<const std::byte> frame, int exceptFd) override;
    bool forwardTo(const PeerId& dst, std::span<const std::byte> frame) override;
    void deliverLocal(const PeerId& from, Bytes payload) override;

private:
    enum class PeerState { Discovered, Connecting, Connected, Disconnected, Lost };

    struct PeerRecord {
        PeerId                       id;
        sockaddr_in                  dialAddr{};
        bool                         haveAddr = false;
        PeerState                    state    = PeerState::Discovered;
        EventLoop::Clock::time_point lastSeen{};
        int                          reconnectAttempts = 0;
        bool                         announced = false;
        PeerConnection*              conn      = nullptr;
    };

    PeerConnection* startDial(PeerRecord& r);
    void            startDialAddr(const sockaddr_in& addr); // address-only (seed)
    void            maybeDial(PeerRecord& r);
    void            scheduleReconnect(PeerRecord& r);
    void            onReconnectTimer(PeerId id);
    void            tick();
    void            scheduleTick();
    void            dialSeeds();
    void            scheduleReap();
    void            reap();
    void            emitConnectedSnapshot();
    void            emitKnownSnapshot();
    std::uint64_t   randNonce();
    bool            keepExisting(PeerConnection* existing, PeerConnection* neu,
                                 const PeerId& peer) const;

    EventLoop&           loop_;
    const MeshConfig&    cfg_;
    const LocalIdentity& self_;
    PeerManagerDelegate& delegate_;
    GossipRouter         router_;

    std::vector<std::unique_ptr<PeerConnection>> conns_;
    std::unordered_map<PeerId, PeerRecord>       records_;
    std::unordered_map<PeerId, PeerConnection*>  estab_; // established, by peer id

    std::vector<PeerConnection*> reap_;
    bool                         reapScheduled_ = false;
    bool                         running_       = false;

    std::mt19937_64 rng_;
};

} // namespace mm
