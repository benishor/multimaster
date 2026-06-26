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

/// Sink for mesh-level events. Implemented by mesh_impl, which forwards to the
/// user's callbacks and maintains thread-safe state snapshots.
class peer_manager_delegate {
public:
    virtual ~peer_manager_delegate() = default;
    virtual void peer_discovered(const peer_id&)                      = 0;
    virtual void peer_connected(const peer_id&)                       = 0;
    virtual void peer_disconnected(const peer_id&)                    = 0;
    virtual void peer_lost(const peer_id&)                            = 0;
    virtual void message_received(const peer_id& from, bytes payload) = 0;
    virtual void on_error(const error&)                                = 0;
    /// New snapshots whenever the relevant set changes (IO thread).
    virtual void connected_snapshot(std::vector<peer_id>)             = 0;
    virtual void known_snapshot(std::vector<peer_id>)                 = 0;
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
        peer_connection*              conn      = nullptr;
    };

    peer_connection* start_dial(peer_record& r);
    void            start_dial_addr(const sockaddr_in& addr); // address-only (seed)
    void            maybe_dial(peer_record& r);
    void            schedule_reconnect(peer_record& r);
    void            on_reconnect_timer(peer_id id);
    void            tick();
    void            schedule_tick();
    void            dial_seeds();
    void            schedule_reap();
    void            reap();
    void            emit_connected_snapshot();
    void            emit_known_snapshot();
    std::uint64_t   rand_nonce();
    bool            keep_existing(peer_connection* existing, peer_connection* neu,
                                 const peer_id& peer) const;

    event_loop&           loop_;
    const mesh_config&    cfg_;
    const local_identity& self_;
    peer_manager_delegate& delegate_;
    gossip_router         router_;

    std::vector<std::unique_ptr<peer_connection>> conns_;
    std::unordered_map<peer_id, peer_record>       records_;
    std::unordered_map<peer_id, peer_connection*>  estab_; // established, by peer id

    std::vector<peer_connection*> reap_;
    bool                         reapScheduled_ = false;
    bool                         running_       = false;

    std::mt19937_64 rng_;
};

} // namespace mm
