#pragma once

#include "event_loop.hpp"
#include "wire.hpp"
#include "multimaster/config.hpp"
#include "multimaster/peer_id.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mm {

class peer_manager_delegate;

/// Tracks mesh-wide membership via adjacency gossip.
///
/// Each node periodically floods a `membership_record` = {origin, version,
/// direct neighbors}. Every node stores the latest record per origin, builds the
/// undirected adjacency graph (its own direct links + all received records), and
/// takes the set of nodes reachable from itself as the membership. Changes fire
/// onMemberJoined / onMemberLeft; stale records expire so a member whose only
/// bridge died eventually disappears.
///
/// Single-threaded: only ever touched on the IO thread, like the rest of the
/// mesh state. The `flood` callback hands an encoded frame to peer_manager for
/// forwarding (except on the inbound link).
class membership {
public:
    using clock = event_loop::clock;
    /// flood(frame, except_fd): send to all established peers except `except_fd`
    /// (-1 = send to all).
    using flood_fn = std::function<void(std::span<const std::byte>, int except_fd)>;

    membership(const peer_id& self, const mesh_config& cfg,
               peer_manager_delegate& delegate, flood_fn flood);

    /// Update this node's direct-neighbor set. If it changed, re-derives
    /// membership and floods a fresh self record.
    void set_local_neighbors(std::vector<peer_id> neighbors, clock::time_point now);

    /// Ingest a membership record received from a peer (arriving on inbound_fd).
    void on_remote_record(const membership_record& rec, int inbound_fd,
                          clock::time_point now);

    /// Periodic upkeep: keepalive re-flood of the self record + expiry of stale
    /// records.
    void tick(clock::time_point now);

    /// Reset all state without firing callbacks (used at shutdown).
    void clear();

private:
    struct entry {
        std::uint64_t        version = 0;
        std::vector<peer_id> neighbors;
        clock::time_point    lastSeen{};
    };

    void flood_self(int except_fd, clock::time_point now);
    void recompute_and_fire();

    const peer_id          self_;
    const mesh_config&     cfg_;
    peer_manager_delegate& delegate_;
    flood_fn               flood_;

    std::vector<peer_id>                  selfNeighbors_;
    std::uint64_t                         selfVersion_;     // monotonic, restart-safe seed
    clock::time_point                     lastSelfFlood_{};

    std::unordered_map<peer_id, entry>    view_;            // latest record per origin
    std::unordered_set<peer_id>           members_;         // currently reachable (excl. self)
};

} // namespace mm
