#include "test_harness.hpp"

#include "event_loop.hpp"
#include "gossip_router.hpp"
#include "multimaster/config.hpp"

#include <string>
#include <vector>

using namespace mm;

namespace {

peer_id id_from(unsigned seed) {
    peer_id id;
    for (auto& b : id.bytes) b = static_cast<std::byte>(seed++);
    return id;
}

message_id msg_from(unsigned seed) {
    message_id m;
    for (auto& b : m.bytes) b = static_cast<std::byte>(seed++);
    return m;
}

// Records what the router decides to do.
struct MockForwarder : forwarder {
    int                                       deliveries = 0;
    int                                       floods     = 0;
    int                                       directSends = 0;
    bool                                      directRouteExists = false;
    std::vector<peer_id>                       deliveredFrom;

    void forward_except(std::span<const std::byte>, int) override { ++floods; }
    bool forward_to(const peer_id&, std::span<const std::byte>) override {
        if (directRouteExists) { ++directSends; return true; }
        return false;
    }
    void deliver_local(const peer_id& from, bytes) override {
        ++deliveries;
        deliveredFrom.push_back(from);
    }
};

data_view broadcast_view(const peer_id& src, const message_id& id, std::uint8_t ttl) {
    data_view v;
    v.src   = src;
    v.dst   = peer_id{}; // zero = broadcast
    v.msgId = id;
    v.ttl   = ttl;
    return v;
}

} // namespace

TEST("duplicate message id is dropped") {
    mesh_config cfg;
    event_loop  loop;
    peer_id     self = id_from(100);
    gossip_router r(cfg, loop, self);
    MockForwarder fwd;

    auto v = broadcast_view(id_from(1), msg_from(50), 4);
    r.on_data(v, /*inboundFd=*/5, fwd);
    r.on_data(v, /*inboundFd=*/5, fwd); // duplicate

    CHECK_EQ(fwd.deliveries, 1); // delivered once only
}

TEST("broadcast delivers locally and floods on") {
    mesh_config cfg;
    event_loop  loop;
    gossip_router r(cfg, loop, id_from(100));
    MockForwarder fwd;

    auto v = broadcast_view(id_from(1), msg_from(60), 4); // ttl 4 => should forward
    r.on_data(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 1);
    CHECK_EQ(fwd.floods, 1);
}

TEST("ttl exhaustion stops forwarding but still delivers") {
    mesh_config cfg;
    event_loop  loop;
    gossip_router r(cfg, loop, id_from(100));
    MockForwarder fwd;

    auto v = broadcast_view(id_from(1), msg_from(70), 1); // ttl 1 => deliver, no forward
    r.on_data(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 1);
    CHECK_EQ(fwd.floods, 0);
}

TEST("own broadcast echo is not delivered") {
    mesh_config cfg;
    event_loop  loop;
    peer_id     self = id_from(100);
    gossip_router r(cfg, loop, self);
    MockForwarder fwd;

    auto v = broadcast_view(self, msg_from(80), 4); // src == self
    r.on_data(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 0);
}

TEST("targeted to self delivers and does not forward") {
    mesh_config cfg;
    event_loop  loop;
    peer_id     self = id_from(100);
    gossip_router r(cfg, loop, self);
    MockForwarder fwd;

    data_view v;
    v.src   = id_from(1);
    v.dst   = self;
    v.msgId = msg_from(90);
    v.ttl   = 4;
    r.on_data(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 1);
    CHECK_EQ(fwd.floods, 0);
    CHECK_EQ(fwd.directSends, 0);
}

TEST("targeted relay prefers direct route") {
    mesh_config cfg;
    event_loop  loop;
    gossip_router r(cfg, loop, id_from(100));
    MockForwarder fwd;
    fwd.directRouteExists = true;

    data_view v;
    v.src   = id_from(1);
    v.dst   = id_from(200); // not us
    v.msgId = msg_from(95);
    v.ttl   = 4;
    r.on_data(v, 5, fwd);
    CHECK_EQ(fwd.deliveries, 0); // relay only
    CHECK_EQ(fwd.directSends, 1);
    CHECK_EQ(fwd.floods, 0);
}

TEST("targeted relay diffuses when no direct route") {
    mesh_config cfg;
    event_loop  loop;
    gossip_router r(cfg, loop, id_from(100));
    MockForwarder fwd;
    fwd.directRouteExists = false;

    data_view v;
    v.src   = id_from(1);
    v.dst   = id_from(200);
    v.msgId = msg_from(96);
    v.ttl   = 4;
    r.on_data(v, 5, fwd);
    CHECK_EQ(fwd.directSends, 0);
    CHECK_EQ(fwd.floods, 1);
}

TEST("size-bounded dedup evicts oldest") {
    mesh_config cfg;
    cfg.maxDedupEntries = 2; // tiny cache
    event_loop  loop;
    gossip_router r(cfg, loop, id_from(100));
    MockForwarder fwd;

    auto m1 = msg_from(10), m2 = msg_from(20), m3 = msg_from(30);
    r.on_data(broadcast_view(id_from(1), m1, 1), 5, fwd); // deliver 1
    r.on_data(broadcast_view(id_from(1), m2, 1), 5, fwd); // deliver 2
    r.on_data(broadcast_view(id_from(1), m3, 1), 5, fwd); // deliver 3, evicts m1
    CHECK_EQ(fwd.deliveries, 3);

    // m1 was evicted, so it is treated as new again.
    r.on_data(broadcast_view(id_from(1), m1, 1), 5, fwd);
    CHECK_EQ(fwd.deliveries, 4);

    // m3 is still cached => dropped.
    r.on_data(broadcast_view(id_from(1), m3, 1), 5, fwd);
    CHECK_EQ(fwd.deliveries, 4);
}

int main() { return mm::test::run(); }
