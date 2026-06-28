// Integration test for the secured (PSK) mesh: nodes sharing a PSK connect and
// exchange encrypted broadcast/targeted traffic, while a node with the wrong PSK
// is never able to join. Wired via seed peers on 127.0.0.1 (no multicast needed).

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
    std::unique_ptr<mesh>    node_mesh;
    std::atomic<int>         connected{0};
    std::mutex               mu;
    std::vector<std::string> received;

    int count_received(const std::string& s) {
        std::lock_guard lk(mu);
        int n = 0;
        for (auto& m : received) if (m == s) ++n;
        return n;
    }
};

mesh_config make_config(uint16_t port, const std::vector<uint16_t>& others, const std::string& psk) {
    mesh_config cfg;
    cfg.groupName         = "secure-smoke-group";
    cfg.bindAddr          = "127.0.0.1";
    cfg.listenPort        = port;
    cfg.multicastIface    = "127.0.0.1";
    cfg.psk               = psk;
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

TEST("secured mesh: encrypted broadcast + targeted between PSK peers") {
    const std::string     psk   = "correct horse battery staple - shared";
    const uint16_t        base  = 47220;
    std::vector<uint16_t> ports = {base, static_cast<uint16_t>(base + 1),
                                   static_cast<uint16_t>(base + 2)};

    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 3; ++i) nodes.push_back(std::make_unique<Node>());

    for (int i = 0; i < 3; ++i) {
        std::vector<uint16_t> others;
        for (int j = 0; j < 3; ++j) if (j != i) others.push_back(ports[j]);

        Node* n      = nodes[i].get();
        n->node_mesh = std::make_unique<mesh>(make_config(ports[i], others, psk));

        callbacks cb;
        cb.onPeerConnected = [n](peer_id) { n->connected++; };
        cb.onMessage       = [n](peer_id, bytes data) {
            std::lock_guard lk(n->mu);
            n->received.push_back(to_str(data));
        };
        n->node_mesh->set_callbacks(std::move(cb));
    }

    for (auto& n : nodes) n->node_mesh->start();

    CHECK(wait_for(
        [&] {
            for (auto& n : nodes) if (n->connected.load() < 1) return false;
            return true;
        },
        6000ms));

    nodes[0]->node_mesh->broadcast(as_bytes("ENC-BCAST"));
    CHECK(wait_for(
        [&] {
            return nodes[1]->count_received("ENC-BCAST") >= 1 &&
                   nodes[2]->count_received("ENC-BCAST") >= 1;
        },
        3000ms));

    peer_id target = nodes[2]->node_mesh->id();
    nodes[0]->node_mesh->send(target, as_bytes("ENC-TARGET"));
    CHECK(wait_for([&] { return nodes[2]->count_received("ENC-TARGET") >= 1; }, 3000ms));
    CHECK_EQ(nodes[1]->count_received("ENC-TARGET"), 0);

    for (auto& n : nodes) n->node_mesh->stop();
}

TEST("secured mesh: wrong PSK cannot join") {
    const uint16_t portA = 47240, portB = 47241;

    auto good = std::make_unique<mesh>(make_config(portA, {portB}, "the-real-psk"));
    auto bad  = std::make_unique<mesh>(make_config(portB, {portA}, "an-impostor-psk"));

    std::atomic<int> goodConnects{0};
    std::atomic<int> badConnects{0};
    {
        callbacks cb;
        cb.onPeerConnected = [&](peer_id) { goodConnects++; };
        good->set_callbacks(std::move(cb));
    }
    {
        callbacks cb;
        cb.onPeerConnected = [&](peer_id) { badConnects++; };
        bad->set_callbacks(std::move(cb));
    }

    good->start();
    bad->start();

    // Give them ample time to (fail to) authenticate in both directions.
    std::this_thread::sleep_for(3000ms);
    CHECK_EQ(goodConnects.load(), 0);
    CHECK_EQ(badConnects.load(), 0);
    CHECK(good->connected_peers().empty());
    CHECK(bad->connected_peers().empty());

    good->stop();
    bad->stop();
}

int main() { return mm::test::run(); }
