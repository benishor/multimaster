// Integration smoke test: several Mesh nodes in one process, wired together via
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

Bytes asBytes(const std::string& s) {
    return Bytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}
std::string toStr(Bytes b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

struct Node {
    std::unique_ptr<Mesh> mesh;
    std::atomic<int>      connected{0};
    std::atomic<int>      disconnects{0};
    std::atomic<bool>     sawLost{false};
    std::mutex            mu;
    std::vector<std::string> received;

    int countReceived(const std::string& s) {
        std::lock_guard lk(mu);
        int n = 0;
        for (auto& m : received) if (m == s) ++n;
        return n;
    }
};

MeshConfig makeConfig(uint16_t port, const std::vector<uint16_t>& others) {
    MeshConfig cfg;
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
bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
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
        n->mesh = std::make_unique<Mesh>(makeConfig(ports[i], others));

        Callbacks cb;
        cb.onPeerConnected    = [n](PeerId) { n->connected++; };
        cb.onPeerDisconnected = [n](PeerId) { n->disconnects++; };
        cb.onPeerLost         = [n](PeerId) { n->sawLost = true; };
        cb.onMessage          = [n](PeerId, Bytes data) {
            std::lock_guard lk(n->mu);
            n->received.push_back(toStr(data));
        };
        n->mesh->setCallbacks(std::move(cb));
    }

    for (auto& n : nodes) n->mesh->start();

    // Wait until every node has at least one peer (graph is connected).
    bool connected = waitFor(
        [&] {
            for (auto& n : nodes) if (n->connected.load() < 1) return false;
            return true;
        },
        5000ms);
    CHECK(connected);

    // Broadcast from node 0 — should reach nodes 1 and 2 (not 0 itself).
    nodes[0]->mesh->broadcast(asBytes("BCAST"));
    bool gotBroadcast = waitFor(
        [&] {
            return nodes[1]->countReceived("BCAST") >= 1 &&
                   nodes[2]->countReceived("BCAST") >= 1;
        },
        3000ms);
    CHECK(gotBroadcast);
    CHECK_EQ(nodes[0]->countReceived("BCAST"), 0); // no self-delivery

    // Targeted send from node 0 to node 2 — only node 2 should deliver it.
    PeerId target = nodes[2]->mesh->id();
    nodes[0]->mesh->send(target, asBytes("TARGET"));
    bool gotTargeted = waitFor([&] { return nodes[2]->countReceived("TARGET") >= 1; }, 3000ms);
    CHECK(gotTargeted);
    CHECK_EQ(nodes[1]->countReceived("TARGET"), 0); // relayed, not delivered
    CHECK_EQ(nodes[0]->countReceived("TARGET"), 0);

    // Stop node 1; nodes 0 and 2 should observe a disconnect, then a loss.
    nodes[1]->mesh->stop();

    bool sawDisconnect = waitFor(
        [&] { return nodes[0]->disconnects.load() >= 1 || nodes[2]->disconnects.load() >= 1; },
        3000ms);
    CHECK(sawDisconnect);

    bool sawLost = waitFor(
        [&] { return nodes[0]->sawLost.load() || nodes[2]->sawLost.load(); },
        5000ms);
    CHECK(sawLost);

    nodes[0]->mesh->stop();
    nodes[2]->mesh->stop();
}

int main() { return mm::test::run(); }
