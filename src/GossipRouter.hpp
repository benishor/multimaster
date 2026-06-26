#pragma once

#include "EventLoop.hpp"
#include "Wire.hpp"
#include "multimaster/Config.hpp"
#include "multimaster/PeerId.hpp"
#include "multimaster/Span.hpp"

#include <cstdint>
#include <deque>
#include <span>
#include <unordered_set>

namespace mm {

/// Abstraction the router uses to actually move bytes; implemented by
/// PeerManager, which owns the live connections.
class Forwarder {
public:
    virtual ~Forwarder() = default;

    /// Send an encoded frame to every established peer except the one on
    /// `exceptFd` (-1 to send to all). Split-horizon for floods.
    virtual void forwardExcept(std::span<const std::byte> frame, int exceptFd) = 0;

    /// Send an encoded frame to `dst` if directly connected. Returns true if a
    /// direct link existed and the frame was queued.
    virtual bool forwardTo(const PeerId& dst, std::span<const std::byte> frame) = 0;

    /// Hand a payload to the local application (Callbacks::onMessage).
    virtual void deliverLocal(const PeerId& from, Bytes payload) = 0;
};

/// Gossip forwarding policy: dedup cache (loop protection), TTL hop-bounding,
/// broadcast flooding, and targeted relay. Holds no connections itself — it
/// drives a Forwarder. Single-threaded (IO thread) like everything else.
class GossipRouter {
public:
    GossipRouter(const MeshConfig& cfg, EventLoop& loop, const PeerId& self);

    /// Arm the periodic dedup-cache eviction sweep.
    void start();

    /// Process a Data frame received on `inboundFd`.
    void onData(const DataView& view, int inboundFd, Forwarder& fwd);

    /// Originate a new broadcast / targeted message from the local application.
    void originateBroadcast(Bytes payload, Forwarder& fwd);
    void originateTargeted(const PeerId& dst, Bytes payload, Forwarder& fwd);

private:
    MessageId nextId();
    bool      seen(const MessageId& id); // returns true if already seen; inserts otherwise
    void      evictSweep();

    const MeshConfig& cfg_;
    EventLoop&        loop_;
    PeerId            self_;
    std::uint64_t     counter_ = 0;

    std::unordered_set<MessageId> dedup_;
    struct Stamp {
        MessageId                    id;
        EventLoop::Clock::time_point at;
    };
    std::deque<Stamp> order_; // FIFO for size + time eviction
};

} // namespace mm
