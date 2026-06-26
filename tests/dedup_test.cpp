#include "test_harness.hpp"

#include "EventLoop.hpp"
#include "GossipRouter.hpp"
#include "multimaster/Config.hpp"

#include <string>
#include <vector>

using namespace mm;

namespace {

PeerId idFrom(unsigned seed) {
    PeerId id;
    for (auto& b : id.bytes) b = static_cast<std::byte>(seed++);
    return id;
}

MessageId msgFrom(unsigned seed) {
    MessageId m;
    for (auto& b : m.bytes) b = static_cast<std::byte>(seed++);
    return m;
}

// Records what the router decides to do.
struct MockForwarder : Forwarder {
    int                                       deliveries = 0;
    int                                       floods     = 0;
    int                                       directSends = 0;
    bool                                      directRouteExists = false;
    std::vector<PeerId>                       deliveredFrom;

    void forwardExcept(std::span<const std::byte>, int) override { ++floods; }
    bool forwardTo(const PeerId&, std::span<const std::byte>) override {
        if (directRouteExists) { ++directSends; return true; }
        return false;
    }
    void deliverLocal(const PeerId& from, Bytes) override {
        ++deliveries;
        deliveredFrom.push_back(from);
    }
};

DataView broadcastView(const PeerId& src, const MessageId& id, std::uint8_t ttl) {
    DataView v;
    v.src   = src;
    v.dst   = PeerId{}; // zero = broadcast
    v.msgId = id;
    v.ttl   = ttl;
    return v;
}

} // namespace

TEST("duplicate message id is dropped") {
    MeshConfig cfg;
    EventLoop  loop;
    PeerId     self = idFrom(100);
    GossipRouter r(cfg, loop, self);
    MockForwarder fwd;

    auto v = broadcastView(idFrom(1), msgFrom(50), 4);
    r.onData(v, /*inboundFd=*/5, fwd);
    r.onData(v, /*inboundFd=*/5, fwd); // duplicate

    CHECK_EQ(fwd.deliveries, 1); // delivered once only
}

TEST("broadcast delivers locally and floods on") {
    MeshConfig cfg;
    EventLoop  loop;
    GossipRouter r(cfg, loop, idFrom(100));
    MockForwarder fwd;

    auto v = broadcastView(idFrom(1), msgFrom(60), 4); // ttl 4 => should forward
    r.onData(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 1);
    CHECK_EQ(fwd.floods, 1);
}

TEST("ttl exhaustion stops forwarding but still delivers") {
    MeshConfig cfg;
    EventLoop  loop;
    GossipRouter r(cfg, loop, idFrom(100));
    MockForwarder fwd;

    auto v = broadcastView(idFrom(1), msgFrom(70), 1); // ttl 1 => deliver, no forward
    r.onData(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 1);
    CHECK_EQ(fwd.floods, 0);
}

TEST("own broadcast echo is not delivered") {
    MeshConfig cfg;
    EventLoop  loop;
    PeerId     self = idFrom(100);
    GossipRouter r(cfg, loop, self);
    MockForwarder fwd;

    auto v = broadcastView(self, msgFrom(80), 4); // src == self
    r.onData(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 0);
}

TEST("targeted to self delivers and does not forward") {
    MeshConfig cfg;
    EventLoop  loop;
    PeerId     self = idFrom(100);
    GossipRouter r(cfg, loop, self);
    MockForwarder fwd;

    DataView v;
    v.src   = idFrom(1);
    v.dst   = self;
    v.msgId = msgFrom(90);
    v.ttl   = 4;
    r.onData(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 1);
    CHECK_EQ(fwd.floods, 0);
    CHECK_EQ(fwd.directSends, 0);
}

TEST("targeted relay prefers direct route") {
    MeshConfig cfg;
    EventLoop  loop;
    GossipRouter r(cfg, loop, idFrom(100));
    MockForwarder fwd;
    fwd.directRouteExists = true;

    DataView v;
    v.src   = idFrom(1);
    v.dst   = idFrom(200); // not us
    v.msgId = msgFrom(95);
    v.ttl   = 4;
    r.onData(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 0); // relay only
    CHECK_EQ(fwd.directSends, 1);
    CHECK_EQ(fwd.floods, 0);
}

TEST("targeted relay diffuses when no direct route") {
    MeshConfig cfg;
    EventLoop  loop;
    GossipRouter r(cfg, loop, idFrom(100));
    MockForwarder fwd;
    fwd.directRouteExists = false;

    DataView v;
    v.src   = idFrom(1);
    v.dst   = idFrom(200);
    v.msgId = msgFrom(96);
    v.ttl   = 4;
    r.onData(v, 5, fwd);
    CHECK_EQ(fwd.directSends, 0);
    CHECK_EQ(fwd.floods, 1);
}

TEST("size-bounded dedup evicts oldest") {
    MeshConfig cfg;
    cfg.maxDedupEntries = 2; // tiny cache
    EventLoop  loop;
    GossipRouter r(cfg, loop, idFrom(100));
    MockForwarder fwd;

    auto m1 = msgFrom(10), m2 = msgFrom(20), m3 = msgFrom(30);
    r.onData(broadcastView(idFrom(1), m1, 1), 5, fwd); // deliver 1
    r.onData(broadcastView(idFrom(1), m2, 1), 5, fwd); // deliver 2
    r.onData(broadcastView(idFrom(1), m3, 1), 5, fwd); // deliver 3, evicts m1
    CHECK_EQ(fwd.deliveries, 3);

    // m1 was evicted, so it is treated as new again.
    r.onData(broadcastView(idFrom(1), m1, 1), 5, fwd);
    CHECK_EQ(fwd.deliveries, 4);

    // m3 is still cached => dropped.
    r.onData(broadcastView(idFrom(1), m3, 1), 5, fwd);
    CHECK_EQ(fwd.deliveries, 4);
}

int main() { return mm::test::run(); }
