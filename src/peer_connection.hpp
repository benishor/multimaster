#pragma once

#include "buffer.hpp"
#include "event_loop.hpp"
#include "socket.hpp"
#include "wire.hpp"
#include "multimaster/config.hpp"
#include "multimaster/events.hpp"
#include "multimaster/peer_id.hpp"

#include <cstdint>
#include <span>

#include <netinet/in.h>

namespace mm {

/// Identity this node advertises to peers in its Hello.
struct LocalIdentity {
    PeerId        nodeId;
    std::string   groupName;
    std::uint8_t  protocolVersion = 1;
    std::uint16_t listenPort      = 0;
};

class PeerConnection;

/// Events surfaced by a PeerConnection to its owner (PeerManager). All are
/// invoked on the IO thread.
class ConnectionListener {
public:
    virtual ~ConnectionListener() = default;

    /// Handshake validated (group/version matched). `inbound` is true if this
    /// node accepted the connection, false if it dialed out.
    virtual void onPeerHandshake(PeerConnection& c, const Hello& peer) = 0;

    /// A Data frame arrived. `view.payload` is valid only for this call.
    virtual void onPeerData(PeerConnection& c, const DataView& view) = 0;

    /// The connection finished (peer closed, error, protocol violation, or
    /// Goodbye). The owner must arrange deferred reaping of `c`.
    virtual void onPeerClosed(PeerConnection& c) = 0;

    virtual void reportError(const Error& e) = 0;
};

/// One TCP connection to a peer. Owns its socket and drives framed, partial
/// reads/writes plus the Hello handshake. Registered with the EventLoop as an
/// IoHandler. Never deletes itself — closure routes through
/// ConnectionListener::onPeerClosed so the owner can reap after the event batch.
class PeerConnection : public IoHandler {
public:
    enum class State { ConnectingOut, Handshaking, Established, Dead };

    /// Construct around an already-connected, accepted socket (inbound).
    PeerConnection(EventLoop& loop, ConnectionListener& listener,
                   const MeshConfig& cfg, const LocalIdentity& self,
                   Socket sock, const sockaddr_in& peerAddr, std::uint64_t nonce);

    /// Construct for an outbound dial; call startConnect() afterwards.
    PeerConnection(EventLoop& loop, ConnectionListener& listener,
                   const MeshConfig& cfg, const LocalIdentity& self,
                   const sockaddr_in& target, std::uint64_t nonce);

    ~PeerConnection() override;

    /// Begin a nonblocking connect (outbound only). Returns false if the socket
    /// or connect() failed outright (the connection is then Dead).
    bool startConnect();

    void onIoEvents(std::uint32_t events) override;

    /// Enqueue an already-encoded frame for transmission (gossip forwarding).
    void sendRaw(std::span<const std::byte> frame);

    /// Send a Heartbeat if the link is idle (used by the liveness tick).
    void maybeHeartbeat(EventLoop::Clock::time_point now,
                        std::chrono::milliseconds interval);

    /// Queue a Goodbye then mark for closure.
    void closeGracefully();

    // --- accessors ----------------------------------------------------------
    [[nodiscard]] State                       state() const noexcept { return state_; }
    [[nodiscard]] bool                        inbound() const noexcept { return inbound_; }
    [[nodiscard]] const PeerId&               peerId() const noexcept { return peerId_; }
    [[nodiscard]] std::uint16_t               peerListenPort() const noexcept { return peerListenPort_; }
    [[nodiscard]] std::uint64_t               localNonce() const noexcept { return localNonce_; }
    [[nodiscard]] std::uint64_t               peerNonce() const noexcept { return peerNonce_; }
    [[nodiscard]] int                         fd() const noexcept { return sock_.get(); }
    [[nodiscard]] const sockaddr_in&          peerAddr() const noexcept { return peerAddr_; }
    [[nodiscard]] EventLoop::Clock::time_point lastRecv() const noexcept { return lastRecv_; }
    [[nodiscard]] EventLoop::Clock::time_point createdAt() const noexcept { return createdAt_; }

private:
    void sendHello();
    void onConnectComplete();
    void doRead();
    void parseInbound();
    void flushOutbound();
    void updateEpoll();
    void fail(ErrorCategory cat, int err, std::string what);
    void markDead();

    EventLoop&          loop_;
    ConnectionListener& listener_;
    const MeshConfig&   cfg_;
    const LocalIdentity& self_;

    Socket      sock_;
    sockaddr_in peerAddr_{};
    bool        inbound_ = false;
    State       state_   = State::Handshaking;

    PeerId        peerId_;
    std::uint16_t peerListenPort_ = 0;
    std::uint64_t localNonce_     = 0;
    std::uint64_t peerNonce_      = 0;

    Buffer in_;
    Buffer out_;
    bool   wantWrite_ = false; // EPOLLOUT currently armed?

    EventLoop::Clock::time_point createdAt_;
    EventLoop::Clock::time_point lastRecv_;
    EventLoop::Clock::time_point lastSend_;

    bool registered_ = false;
    bool dead_       = false;
};

} // namespace mm
