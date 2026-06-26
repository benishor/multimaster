// Integration smoke test: several mesh nodes in one process, wired together via
// seed peers on 127.0.0.1 (deterministic; does not depend on multicast working
// in the test environment). Verifies broadcast reaches all peers, targeted send
// reaches exactly one, and that stopping a node drives disconnect + lost events.

#include "test_harness.hpp"

#include <multimaster/multimaster.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mm;
using namespace std::chrono_literals;

namespace {

bytes as_bytes(const std::string& s) {
    return bytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}
std::string to_str(bytes b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

struct Node {
    std::unique_ptr<mesh> node_mesh;
    std::atomic<int>      connected{0};
    std::atomic<int>      disconnects{0};
    std::atomic<bool>     sawLost{false};
    std::mutex            mu;
    std::vector<std::string> received;

    int count_received(const std::string& s) {
        std::lock_guard lk(mu);
        int n = 0;
        for (auto& m : received) if (m == s) ++n;
        return n;
    }
};

mesh_config make_config(uint16_t port, const std::vector<uint16_t>& others) {
    mesh_config cfg;
    cfg.groupName        = "smoke-test-group";
    cfg.bindAddr         = "127.0.0.1";
    cfg.listenPort       = port;
    cfg.multicastIface   = "127.0.0.1";
    cfg.heartbeatInterval = 300ms;
    cfg.heartbeatTimeout  = 1500ms;
    cfg.peerLostTimeout   = 2000ms;
    cfg.reconnectBase     = 200ms;
    for (uint16_t p : others) cfg.seedPeers.push_back({"127.0.0.1", p});
    return cfg;
}

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(25ms);
    }
    return pred();
}

} // namespace

TEST("three-node mesh: broadcast, targeted, disconnect, lost") {
    const uint16_t base = 47120;
    std::vector<uint16_t> ports = {base, static_cast<uint16_t>(base + 1),
                                   static_cast<uint16_t>(base + 2)};

    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 3; ++i) nodes.push_back(std::make_unique<Node>());

    for (int i = 0; i < 3; ++i) {
        std::vector<uint16_t> others;
        for (int j = 0; j < 3; ++j) if (j != i) others.push_back(ports[j]);

        Node* n = nodes[i].get();
        n->node_mesh = std::make_unique<mesh>(make_config(ports[i], others));

        callbacks cb;
        cb.onPeerConnected    = [n](peer_id) { n->connected++; };
        cb.onPeerDisconnected = [n](peer_id) { n->disconnects++; };
        cb.onPeerLost         = [n](peer_id) { n->sawLost = true; };
        cb.onMessage          = [n](peer_id, bytes data) {
            std::lock_guard lk(n->mu);
            n->received.push_back(to_str(data));
        };
        n->node_mesh->set_callbacks(std::move(cb));
    }

    for (auto& n : nodes) n->node_mesh->start();

    // Wait until every node has at least one peer (graph is connected).
    bool connected = wait_for(
        [&] {
            for (auto& n : nodes) if (n->connected.load() < 1) return false;
            return true;
        },
        5000ms);
    CHECK(connected);

    // Broadcast from node 0 — should reach nodes 1 and 2 (not 0 itself).
    nodes[0]->node_mesh->broadcast(as_bytes("BCAST"));
    bool gotBroadcast = wait_for(
        [&] {
            return nodes[1]->count_received("BCAST") >= 1 &&
                   nodes[2]->count_received("BCAST") >= 1;
        },
        3000ms);
    CHECK(gotBroadcast);
    CHECK_EQ(nodes[0]->count_received("BCAST"), 0); // no self-delivery

    // Targeted send from node 0 to node 2 — only node 2 should deliver it.
    peer_id target = nodes[2]->node_mesh->id();
    nodes[0]->node_mesh->send(target, as_bytes("TARGET"));
    bool gotTargeted = wait_for([&] { return nodes[2]->count_received("TARGET") >= 1; }, 3000ms);
    CHECK(gotTargeted);
    CHECK_EQ(nodes[1]->count_received("TARGET"), 0); // relayed, not delivered
    CHECK_EQ(nodes[0]->count_received("TARGET"), 0);

    // Stop node 1; nodes 0 and 2 should observe a disconnect, then a loss.
    nodes[1]->node_mesh->stop();

    bool sawDisconnect = wait_for(
        [&] { return nodes[0]->disconnects.load() >= 1 || nodes[2]->disconnects.load() >= 1; },
        3000ms);
    CHECK(sawDisconnect);

    bool sawLost = wait_for(
        [&] { return nodes[0]->sawLost.load() || nodes[2]->sawLost.load(); },
        5000ms);
    CHECK(sawLost);

    nodes[0]->node_mesh->stop();
    nodes[2]->node_mesh->stop();
}

// Two nodes that can ONLY find each other via a configured static (internet-
// style) peer: discovery is neutralized by giving them distinct multicast ports,
// so a connection proves the static-peer path works. Also verifies persistent
// reconnection after the static peer restarts.
TEST("static peer: connect, message, persistent reconnect") {
    const uint16_t portA = 47140, portB = 47141;

    auto cfgA = [&] {
        mesh_config c;
        c.groupName        = "static-test-group";
        c.bindAddr         = "127.0.0.1";
        c.listenPort       = portA;
        c.multicastPort    = 50140;          // isolated from B's discovery
        c.multicastIface   = "127.0.0.1";
        c.heartbeatInterval = 300ms;
        c.heartbeatTimeout  = 1500ms;
        c.reconnectBase     = 200ms;
        c.reconnectMax      = 1500ms;
        return c;
    };

    mesh_config cfgB;
    cfgB.groupName        = "static-test-group";
    cfgB.bindAddr         = "127.0.0.1";
    cfgB.listenPort       = portB;
    cfgB.multicastPort    = 50141;           // distinct => no multicast pairing
    cfgB.multicastIface   = "127.0.0.1";
    cfgB.heartbeatInterval = 300ms;
    cfgB.heartbeatTimeout  = 1500ms;
    cfgB.reconnectBase     = 200ms;
    cfgB.reconnectMax      = 1500ms;
    cfgB.staticPeers.push_back({"127.0.0.1", portA}); // A reachable only as a static peer

    std::atomic<int> bConnects{0};
    std::atomic<int> aMessages{0};

    auto node_b = std::make_unique<mesh>(cfgB);
    {
        callbacks cb;
        cb.onPeerConnected = [&](peer_id) { bConnects++; };
        node_b->set_callbacks(std::move(cb));
    }
    node_b->start();

    auto start_a = [&] {
        auto a = std::make_unique<mesh>(cfgA());
        callbacks cb;
        cb.onMessage = [&](peer_id, bytes) { aMessages++; };
        a->set_callbacks(std::move(cb));
        a->start();
        return a;
    };
    auto node_a = start_a();

    // B should dial A via the static endpoint and connect.
    CHECK(wait_for([&] { return node_b->connected_peers().size() >= 1; }, 4000ms));

    // Data flows over the static-established link.
    node_b->broadcast(as_bytes("via-static"));
    CHECK(wait_for([&] { return aMessages.load() >= 1; }, 3000ms));

    // A goes away; B observes the drop.
    node_a.reset();
    CHECK(wait_for([&] { return node_b->connected_peers().empty(); }, 4000ms));

    // A comes back on the same endpoint; B must persistently reconnect.
    node_a = start_a();
    CHECK(wait_for([&] { return node_b->connected_peers().size() >= 1; }, 6000ms));
    CHECK(bConnects.load() >= 2); // connected at least twice => reconnect happened

    node_a.reset();
    node_b->stop();
}

int main() { return mm::test::run(); }
