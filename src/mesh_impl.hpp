#pragma once

#include "discovery.hpp"
#include "event_loop.hpp"
#include "listener.hpp"
#include "peer_connection.hpp" // LocalIdentity
#include "peer_manager.hpp"
#include "multimaster/config.hpp"
#include "multimaster/events.hpp"
#include "multimaster/peer_id.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mm {

/// The full mesh node implementation, hidden behind the Mesh pimpl. Owns the
/// IO thread and all subsystems; implements PeerManagerDelegate to bridge
/// internal events to the user's Callbacks and to maintain thread-safe
/// snapshots of mesh state.
class MeshImpl : public PeerManagerDelegate {
public:
    explicit MeshImpl(MeshConfig cfg);
    ~MeshImpl() override;

    void setCallbacks(Callbacks cb);
    void start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept { return running_.load(); }

    void broadcast(Bytes data);
    void send(const PeerId& dst, Bytes data);

    [[nodiscard]] PeerId   id() const noexcept { return cfg_.nodeId; }
    [[nodiscard]] std::uint16_t listenPort() const noexcept { return listenPort_.load(); }
    [[nodiscard]] std::vector<PeerId> connectedPeers() const;
    [[nodiscard]] std::vector<PeerId> knownPeers() const;

    // PeerManagerDelegate
    void peerDiscovered(const PeerId&) override;
    void peerConnected(const PeerId&) override;
    void peerDisconnected(const PeerId&) override;
    void peerLost(const PeerId&) override;
    void messageReceived(const PeerId& from, Bytes payload) override;
    void error(const Error&) override;
    void connectedSnapshot(std::vector<PeerId>) override;
    void knownSnapshot(std::vector<PeerId>) override;

private:
    struct Command {
        enum class Type { Broadcast, Targeted, Stop } type;
        PeerId                 dst;
        std::vector<std::byte> payload;
    };

    void post(Command&& c);
    void drainMailbox();

    MeshConfig    cfg_;
    LocalIdentity self_;
    Callbacks     cb_;

    EventLoop                    loop_;
    std::unique_ptr<PeerManager> peers_;
    std::unique_ptr<Listener>    listener_;
    std::unique_ptr<Discovery>   discovery_;

    std::thread       ioThread_;
    std::thread::id   ioThreadId_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    std::atomic<std::uint16_t> listenPort_{0};

    std::mutex          mailboxMu_;
    std::deque<Command> mailbox_;

    mutable std::mutex                       snapMu_;
    std::shared_ptr<const std::vector<PeerId>> connectedSnap_;
    std::shared_ptr<const std::vector<PeerId>> knownSnap_;
};

} // namespace mm
