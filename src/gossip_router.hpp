// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "event_loop.hpp"
#include "wire.hpp"
#include "multimaster/config.hpp"
#include "multimaster/peer_id.hpp"
#include "multimaster/span.hpp"

#include <cstdint>
#include <deque>
#include <span>
#include <unordered_set>

namespace mm {

/// Abstraction the router uses to actually move bytes; implemented by
/// peer_manager, which owns the live connections.
class forwarder {
public:
    virtual ~forwarder() = default;

    /// Send an encoded frame to every established peer except the one on
    /// `exceptFd` (-1 to send to all). Split-horizon for floods.
    virtual void forward_except(std::span<const std::byte> frame, int exceptFd) = 0;

    /// Send an encoded frame to `dst` if directly connected. Returns true if a
    /// direct link existed and the frame was queued.
    virtual bool forward_to(const peer_id& dst, std::span<const std::byte> frame) = 0;

    /// Hand a payload to the local application (callbacks::onMessage).
    virtual void deliver_local(const peer_id& from, bytes payload) = 0;
};

/// Gossip forwarding policy: dedup cache (loop protection), TTL hop-bounding,
/// broadcast flooding, and targeted relay. Holds no connections itself — it
/// drives a forwarder. Single-threaded (IO thread) like everything else.
class gossip_router {
public:
    gossip_router(const mesh_config& cfg, event_loop& loop, const peer_id& self);

    /// Arm the periodic dedup-cache eviction sweep.
    void start();

    /// Process a Data frame received on `inboundFd`.
    void on_data(const data_view& view, int inboundFd, forwarder& fwd);

    /// Originate a new broadcast / targeted message from the local application.
    void originate_broadcast(bytes payload, forwarder& fwd);
    void originate_targeted(const peer_id& dst, bytes payload, forwarder& fwd);

private:
    message_id next_id();
    bool      seen(const message_id& id); // returns true if already seen; inserts otherwise
    void      evict_sweep();

    const mesh_config& cfg_;
    event_loop&        loop_;
    peer_id            self_;
    std::uint64_t     counter_ = 0;

    std::unordered_set<message_id> dedup_;
    struct stamp {
        message_id                    id;
        event_loop::clock::time_point at;
    };
    std::deque<stamp> order_; // FIFO for size + time eviction
};

} // namespace mm
