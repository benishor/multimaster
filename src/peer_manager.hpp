// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "event_loop.hpp"
#include "gossip_router.hpp"
#include "membership.hpp"
#include "peer_connection.hpp"
#include "socket.hpp"
#include "wire.hpp"
#include "multimaster/config.hpp"
#include "multimaster/events.hpp"
#include "multimaster/peer_id.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <netinet/in.h>

namespace mm {

/// Sink for mesh-level events. Implemented by mesh_impl, which forwards to the
/// user's callbacks and maintains thread-safe state snapshots.
class peer_manager_delegate {
public:
    virtual ~peer_manager_delegate() = default;
    virtual void peer_discovered(const peer_id&)                      = 0;
    virtual void peer_connected(const peer_id&)                       = 0;
    virtual void peer_disconnected(const peer_id&)                    = 0;
    virtual void peer_lost(const peer_id&)                            = 0;
    virtual void member_joined(const peer_id&)                        = 0;
    virtual void member_left(const peer_id&)                          = 0;
    virtual void message_received(const peer_id& from, bytes payload) = 0;
    virtual void on_error(const error&)                                = 0;
    /// New snapshots whenever the relevant set changes (IO thread).
    virtual void connected_snapshot(std::vector<peer_id>)             = 0;
    virtual void known_snapshot(std::vector<peer_id>)                 = 0;
    virtual void members_snapshot(std::vector<peer_id>)               = 0;
    virtual void names_snapshot(std::unordered_map<peer_id, std::string>) = 0;
};

/// Owns all peer connections and the mesh's logical state: discovery → dial →
/// handshake → dial-race resolution → liveness → reconnect/backoff → lost.
/// Acts as the connection_listener for every peer_connection and as the forwarder
/// for the gossip_router.
class peer_manager : public connection_listener, public forwarder {
public:
    peer_manager(event_loop& loop, const mesh_config& cfg, const local_identity& self,
                peer_manager_delegate& delegate);
    ~peer_manager() override;

    void start();
    void shutdown(); // send Goodbye to all and tear down

    // From discovery.
    void on_discovered(const announce& a, const sockaddr_in& src);
    // From listener.
    void accept_connection(socket sock, const sockaddr_in& peerAddr);

    // Origination from the public API (runs on IO thread).
    void originate_broadcast(bytes payload) { router_.originate_broadcast(payload, *this); }
    void originate_targeted(const peer_id& dst, bytes payload) {
        router_.originate_targeted(dst, payload, *this);
    }

    // connection_listener
    void on_peer_handshake(peer_connection& c, const hello& peer) override;
    void on_peer_data(peer_connection& c, const data_view& view) override;
    void on_peer_membership(peer_connection& c, const membership_record& rec) override;
    void on_peer_identity(peer_connection& c, const identity_record& rec) override;
    void on_peer_closed(peer_connection& c) override;
    void report_error(const error& e) override;

    // forwarder
    void forward_except(std::span<const std::byte> frame, int exceptFd) override;
    bool forward_to(const peer_id& dst, std::span<const std::byte> frame) override;
    void deliver_local(const peer_id& from, bytes payload) override;

private:
    enum class peer_state { Discovered, Connecting, Connected, Disconnected, Lost };

    struct peer_record {
        peer_id                       id;
        sockaddr_in                  dialAddr{};
        bool                         haveAddr = false;
        peer_state                    state    = peer_state::Discovered;
        event_loop::clock::time_point lastSeen{};
        int                          reconnectAttempts = 0;
        bool                         announced = false;
        bool                         isStatic  = false; // a configured static peer
        peer_connection*              conn      = nullptr;
    };

    // A configured static (typically internet) peer whose connection is kept up
    // persistently. Tracked by endpoint because the peer_id is unknown until the
    // first handshake; the host is re-resolved on every dial attempt.
    struct static_target {
        std::string                   host;
        std::uint16_t                 port = 0;
        std::optional<peer_id>        learnedId;          // set once handshaked
        int                           attempts = 0;
        event_loop::clock::time_point nextAttempt{};      // backoff gate
        peer_connection*              dialing = nullptr;   // in-flight dial, if any
    };

    peer_connection* start_dial(peer_record& r);
    void            start_dial_addr(const sockaddr_in& addr); // address-only (seed)
    void            maybe_dial(peer_record& r);
    void            schedule_reconnect(peer_record& r);
    void            on_reconnect_timer(peer_id id);
    void            tick();
    void            schedule_tick();
    void            dial_seeds();
    void            maintain_static_peers();
    void            start_static_dial(std::size_t targetIdx);
    bool            resolve_endpoint(const std::string& host, std::uint16_t port,
                                     sockaddr_in& out) const;
    void            schedule_reap();
    void            reap();
    void            update_local_membership();
    void            emit_connected_snapshot();
    void            emit_known_snapshot();
    void            emit_names_snapshot();
    // Signed node-name gossip (parallels membership).
    identity_record        make_self_id_record();
    void                   flood_self_identity(int except_fd);
    std::vector<std::byte> id_record_msg(std::uint64_t version, const std::string& name) const;
    std::uint64_t   rand_nonce();
    bool            keep_existing(peer_connection* existing, peer_connection* neu,
                                 const peer_id& peer) const;

    event_loop&           loop_;
    const mesh_config&    cfg_;
    const local_identity& self_;
    peer_manager_delegate& delegate_;
    gossip_router         router_;
    membership            membership_;

    std::vector<std::unique_ptr<peer_connection>> conns_;
    std::unordered_map<peer_id, peer_record>       records_;
    std::unordered_map<peer_id, peer_connection*>  estab_; // established, by peer id

    std::vector<static_target>                       static_targets_;
    std::unordered_map<peer_connection*, std::size_t> static_dial_conn_; // dial -> target idx

    std::unordered_map<peer_id, std::string>         names_;      // verified peer names by id
    std::unordered_map<peer_id, std::uint64_t>       idVersions_; // identity-gossip dedup
    std::uint64_t                                    selfIdVersion_ = 0;
    event_loop::clock::time_point                    lastIdFlood_{};

    std::vector<peer_connection*> reap_;
    bool                         reapScheduled_ = false;
    bool                         running_       = false;

    std::mt19937_64 rng_;
};

} // namespace mm
