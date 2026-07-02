// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

// Integration tests for self-certifying node identity: derived/stable ids,
// signed names + local labels, allowlist revocation, and require-identity.
// Wired via seed peers on 127.0.0.1 (no multicast needed).

#include "test_harness.hpp"

#include "crypto.hpp" // derive expected ids/keys from seeds

#include <multimaster/multimaster.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace mm;
using namespace std::chrono_literals;

namespace {

std::string hex(const std::byte* p, std::size_t n) {
    static const char* k = "0123456789abcdef";
    std::string        s;
    for (std::size_t i = 0; i < n; ++i) {
        auto v = static_cast<unsigned>(p[i]);
        s.push_back(k[v >> 4]);
        s.push_back(k[v & 0xF]);
    }
    return s;
}

// A 32-byte seed filled from a single byte pattern, as hex (for identitySeedHex).
crypto::seed32 make_seed(unsigned char fill) {
    crypto::seed32 s{};
    for (auto& b : s) b = static_cast<std::byte>(fill);
    return s;
}
std::string seed_hex(const crypto::seed32& s) { return hex(s.data(), s.size()); }

// The peer_id and public-key hex a given seed yields (mirrors the library).
peer_id id_of(const crypto::seed32& s) {
    auto    kp  = crypto::identity_from_seed(s);
    auto    raw = crypto::id_from_identity(kp.pk);
    peer_id id;
    std::copy(raw.begin(), raw.end(), id.bytes.begin());
    return id;
}
std::string pubkey_hex(const crypto::seed32& s) {
    auto kp = crypto::identity_from_seed(s);
    return hex(kp.pk.data(), kp.pk.size());
}

mesh_config base_config(uint16_t port, const std::vector<uint16_t>& peers) {
    mesh_config c;
    c.groupName         = "identity-test";
    c.bindAddr          = "127.0.0.1";
    c.listenPort        = port;
    c.multicastIface    = "127.0.0.1";
    c.psk               = "shared-group-secret"; // identity requires a PSK
    c.heartbeatInterval = 300ms;
    c.heartbeatTimeout  = 1500ms;
    c.reconnectBase     = 200ms;
    for (uint16_t p : peers) c.seedPeers.push_back({"127.0.0.1", p});
    return c;
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

TEST("identity: derived id, mutual trust, signed name + local label") {
    auto seedA = make_seed(0xA1), seedB = make_seed(0xB2);

    auto cfgA = base_config(47260, {47261});
    cfgA.identitySeedHex = seed_hex(seedA);
    cfgA.nodeName        = "alice";
    cfgA.trustedKeys.push_back({pubkey_hex(seedB), ""}); // trust B, no local label

    auto cfgB = base_config(47261, {47260});
    cfgB.identitySeedHex = seed_hex(seedB);
    cfgB.nodeName        = "bob";
    cfgB.trustedKeys.push_back({pubkey_hex(seedA), "ALICE-LABEL"}); // trust A, with label

    auto a = std::make_unique<mesh>(cfgA);
    auto b = std::make_unique<mesh>(cfgB);
    a->set_callbacks({});
    b->set_callbacks({});
    a->start();
    b->start();

    // nodeId is derived from the identity key (stable across runs).
    CHECK(a->id() == id_of(seedA));
    CHECK(b->id() == id_of(seedB));

    CHECK(wait_for([&] { return !a->connected_peers().empty() && !b->connected_peers().empty(); },
                   6000ms));

    // The connected peer's id matches the hash of its identity key.
    CHECK(a->connected_peers().front() == id_of(seedB));
    CHECK(b->connected_peers().front() == id_of(seedA));

    // A has no local label for B => sees B's signed self-name.
    CHECK_EQ(a->node_name(id_of(seedB)), std::string("bob"));
    // B has a local label for A => it overrides A's signed self-name.
    CHECK_EQ(b->node_name(id_of(seedA)), std::string("ALICE-LABEL"));

    a->stop();
    b->stop();
}

TEST("identity: untrusted key is refused (revocation)") {
    // A revoked node is one removed from the allowlists of the honest nodes. Here
    // neither A nor B lists the other (both trust only some third key X), so each
    // refuses the other and no connection is admitted on either side. (Note:
    // allowlists are checked per-side, so one-sided trust would let the trusting
    // side briefly half-open before the other drops it — revocation is mesh-wide.)
    auto seedA = make_seed(0x11), seedB = make_seed(0x22), seedX = make_seed(0x33);

    auto cfgA = base_config(47263, {47264});
    cfgA.identitySeedHex = seed_hex(seedA);
    cfgA.trustedKeys.push_back({pubkey_hex(seedX), ""}); // A trusts X, NOT B

    auto cfgB = base_config(47264, {47263});
    cfgB.identitySeedHex = seed_hex(seedB);
    cfgB.trustedKeys.push_back({pubkey_hex(seedX), ""}); // B trusts X, NOT A

    std::atomic<int> aConn{0}, bConn{0};
    auto             a = std::make_unique<mesh>(cfgA);
    auto             b = std::make_unique<mesh>(cfgB);
    {
        callbacks cb;
        cb.onPeerConnected = [&](peer_id) { aConn++; };
        a->set_callbacks(std::move(cb));
    }
    {
        callbacks cb;
        cb.onPeerConnected = [&](peer_id) { bConn++; };
        b->set_callbacks(std::move(cb));
    }
    a->start();
    b->start();

    std::this_thread::sleep_for(3000ms);
    CHECK_EQ(aConn.load(), 0);
    CHECK_EQ(bConn.load(), 0);
    CHECK(a->connected_peers().empty());
    CHECK(b->connected_peers().empty());

    a->stop();
    b->stop();
}

TEST("identity: requireIdentity refuses a peer with no identity") {
    auto seedA = make_seed(0x44);

    auto cfgA = base_config(47266, {47267});
    cfgA.identitySeedHex = seed_hex(seedA);
    cfgA.requireIdentity = true; // (default) reject identity-less peers

    auto cfgB = base_config(47267, {47266}); // same PSK, but NO identity configured

    std::atomic<int> aConn{0};
    auto             a = std::make_unique<mesh>(cfgA);
    auto             b = std::make_unique<mesh>(cfgB);
    {
        callbacks cb;
        cb.onPeerConnected = [&](peer_id) { aConn++; };
        a->set_callbacks(std::move(cb));
    }
    b->set_callbacks({});
    a->start();
    b->start();

    std::this_thread::sleep_for(3000ms);
    CHECK_EQ(aConn.load(), 0);
    CHECK(a->connected_peers().empty());
    CHECK(b->connected_peers().empty());

    a->stop();
    b->stop();
}

TEST("identity: name propagates mesh-wide via gossip (B - A - C)") {
    // B and C are not direct neighbors; each only dials A (distinct multicast
    // ports neutralize discovery). B should still learn C's signed name through A.
    auto seedA = make_seed(0x5A), seedB = make_seed(0x5B), seedC = make_seed(0x5C);

    auto cfg = [](uint16_t port, uint16_t mport, const crypto::seed32& seed,
                  const std::string& name, std::vector<uint16_t> statics) {
        mesh_config c;
        c.groupName          = "identity-gossip-test";
        c.bindAddr           = "127.0.0.1";
        c.listenPort         = port;
        c.multicastPort      = mport; // distinct per node => no multicast pairing
        c.multicastIface     = "127.0.0.1";
        c.psk                = "gossip-psk";
        c.identitySeedHex    = seed_hex(seed);
        c.nodeName           = name;
        c.heartbeatInterval  = 300ms;
        c.heartbeatTimeout   = 1500ms;
        c.reconnectBase      = 200ms;
        c.membershipInterval = 400ms;
        c.membershipTimeout  = 2000ms;
        for (uint16_t p : statics) c.staticPeers.push_back({"127.0.0.1", p});
        return c;
    };

    auto a = std::make_unique<mesh>(cfg(47270, 50270, seedA, "node-a", {}));
    auto b = std::make_unique<mesh>(cfg(47271, 50271, seedB, "node-b", {47270}));
    auto c = std::make_unique<mesh>(cfg(47272, 50272, seedC, "node-c", {47270}));
    a->set_callbacks({});
    b->set_callbacks({});
    c->set_callbacks({});
    a->start();
    b->start();
    c->start();

    peer_id cId = id_of(seedC);
    // B learns node-c's name purely via A's relayed identity gossip.
    CHECK(wait_for([&] { return b->node_name(cId) == std::string("node-c"); }, 6000ms));
    // ...while C is not a direct neighbor of B (only A is).
    CHECK_EQ(b->connected_peers().size(), std::size_t{1});
    CHECK(b->connected_peers().front() == id_of(seedA));

    a->stop();
    b->stop();
    c->stop();
}

int main() { return mm::test::run(); }
