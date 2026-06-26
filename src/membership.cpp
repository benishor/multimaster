#include "membership.hpp"

#include "peer_manager.hpp" // peer_manager_delegate

#include <algorithm>
#include <chrono>
#include <queue>

namespace mm {

membership::membership(const peer_id& self, const mesh_config& cfg,
                       peer_manager_delegate& delegate, flood_fn flood)
    : self_(self), cfg_(cfg), delegate_(delegate), flood_(std::move(flood)) {
    // Seed the version with the wall-clock time (ms) so that records minted after
    // a restart outrank any still circulating from a previous incarnation. Used
    // only as a monotonic ordering tag, never for timing.
    selfVersion_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void membership::set_local_neighbors(std::vector<peer_id> neighbors, clock::time_point now) {
    std::sort(neighbors.begin(), neighbors.end());
    if (neighbors == selfNeighbors_) return; // no change
    selfNeighbors_ = std::move(neighbors);
    recompute_and_fire();
    flood_self(-1, now);
}

void membership::on_remote_record(const membership_record& rec, int inbound_fd,
                                  clock::time_point now) {
    if (rec.origin == self_) return; // never ingest our own gossip echoed back

    auto it = view_.find(rec.origin);
    if (it != view_.end() && rec.version <= it->second.version) {
        return; // stale or duplicate — drop (also stops flood loops)
    }

    entry& e   = view_[rec.origin];
    e.version  = rec.version;
    e.neighbors = rec.neighbors;
    e.lastSeen = now;

    recompute_and_fire();

    // Relay onward to every neighbor except the one it came in on.
    flood_(encode_membership(rec), inbound_fd);
}

void membership::tick(clock::time_point now) {
    // Expire records not refreshed within the timeout.
    bool expired = false;
    for (auto it = view_.begin(); it != view_.end();) {
        if (now - it->second.lastSeen > cfg_.membershipTimeout) {
            it = view_.erase(it);
            expired = true;
        } else {
            ++it;
        }
    }
    if (expired) recompute_and_fire();

    // Keepalive re-flood of our own record so remote nodes don't expire us.
    if (now - lastSelfFlood_ >= cfg_.membershipInterval) {
        flood_self(-1, now);
    }
}

void membership::clear() {
    selfNeighbors_.clear();
    view_.clear();
    members_.clear();
    lastSelfFlood_ = {};
}

void membership::flood_self(int except_fd, clock::time_point now) {
    membership_record rec;
    rec.origin    = self_;
    rec.version   = ++selfVersion_;
    rec.neighbors = selfNeighbors_;
    lastSelfFlood_ = now;
    flood_(encode_membership(rec), except_fd);
}

void membership::recompute_and_fire() {
    // Build the undirected adjacency graph: our direct links + every record.
    std::unordered_map<peer_id, std::unordered_set<peer_id>> adj;
    auto add_edge = [&](const peer_id& a, const peer_id& b) {
        adj[a].insert(b);
        adj[b].insert(a);
    };
    for (const auto& n : selfNeighbors_) add_edge(self_, n);
    for (const auto& [origin, e] : view_) {
        for (const auto& n : e.neighbors) add_edge(origin, n);
    }

    // BFS from self to find the reachable component.
    std::unordered_set<peer_id> reachable;
    std::queue<peer_id>         work;
    work.push(self_);
    std::unordered_set<peer_id> visited{self_};
    while (!work.empty()) {
        peer_id u = work.front();
        work.pop();
        auto it = adj.find(u);
        if (it == adj.end()) continue;
        for (const auto& v : it->second) {
            if (visited.insert(v).second) {
                reachable.insert(v);
                work.push(v);
            }
        }
    }
    reachable.erase(self_);

    // Diff against the previous membership and fire callbacks.
    for (const auto& id : reachable) {
        if (members_.find(id) == members_.end()) delegate_.member_joined(id);
    }
    for (const auto& id : members_) {
        if (reachable.find(id) == reachable.end()) delegate_.member_left(id);
    }

    if (reachable != members_) {
        members_ = std::move(reachable);
        delegate_.members_snapshot(std::vector<peer_id>(members_.begin(), members_.end()));
    }
}

} // namespace mm
