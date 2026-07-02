// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

// Unit tests for the crypto layer: PSK key agreement, AEAD record round-trips,
// tamper / wrong-key rejection, and discovery MAC verification.

#include "test_harness.hpp"

#include "crypto.hpp"

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace mm;

namespace {

std::vector<std::byte> bytes_of(const std::string& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}

// Run the two-sided handshake for a given pair of PSKs/groups and report whether
// both sides authenticated each other.
bool handshake_pair(const std::string& pskA, const std::string& pskB, const std::string& group,
                    crypto::secure_session& a, crypto::secure_session& b) {
    auto kA = crypto::derive_group_key(pskA);
    auto kB = crypto::derive_group_key(pskB);
    auto ea = crypto::gen_ephemeral();
    auto eb = crypto::gen_ephemeral();

    crypto::handshake_result ra, rb;
    if (!crypto::do_handshake(ea, eb.pk, kA, group, ra)) return false;
    if (!crypto::do_handshake(eb, ea.pk, kB, group, rb)) return false;

    // Each side checks the other's confirmation tag.
    bool ok = crypto::verify_tag(ra.expectedPeerTag, rb.myConfirmTag) &&
              crypto::verify_tag(rb.expectedPeerTag, ra.myConfirmTag);
    a = std::move(ra.session);
    b = std::move(rb.session);
    return ok;
}

} // namespace

TEST("crypto init succeeds") { CHECK(crypto::init()); }

TEST("matching PSK: handshake authenticates and session round-trips") {
    crypto::secure_session a, b;
    CHECK(handshake_pair("hunter2-shared-secret", "hunter2-shared-secret", "g", a, b));

    // A -> B
    auto msg     = bytes_of("the quick brown fox");
    auto record  = a.seal(std::span<const std::byte>(msg.data(), msg.size()));
    // strip the 4-byte length prefix that seal() prepends
    std::span<const std::byte> ct(record.data() + 4, record.size() - 4);
    std::vector<std::byte> out;
    CHECK(b.open(ct, out));
    CHECK_EQ(out.size(), msg.size());
    CHECK(std::memcmp(out.data(), msg.data(), msg.size()) == 0);

    // B -> A (independent direction/counter)
    auto msg2    = bytes_of("jumps over the lazy dog");
    auto record2 = b.seal(std::span<const std::byte>(msg2.data(), msg2.size()));
    std::span<const std::byte> ct2(record2.data() + 4, record2.size() - 4);
    std::vector<std::byte> out2;
    CHECK(a.open(ct2, out2));
    CHECK(std::memcmp(out2.data(), msg2.data(), msg2.size()) == 0);
}

TEST("tampered record fails to open") {
    crypto::secure_session a, b;
    CHECK(handshake_pair("a-good-psk", "a-good-psk", "g", a, b));
    auto msg    = bytes_of("authentic");
    auto record = a.seal(std::span<const std::byte>(msg.data(), msg.size()));
    record[record.size() - 1] ^= std::byte{0x01}; // flip a tag bit
    std::span<const std::byte> ct(record.data() + 4, record.size() - 4);
    std::vector<std::byte> out;
    CHECK(!b.open(ct, out));
}

TEST("counter desync (dropped record) fails to open") {
    crypto::secure_session a, b;
    CHECK(handshake_pair("psk-counters", "psk-counters", "g", a, b));
    auto m1 = bytes_of("first");
    auto m2 = bytes_of("second");
    (void)a.seal(std::span<const std::byte>(m1.data(), m1.size())); // advances A's counter, not sent
    auto r2 = a.seal(std::span<const std::byte>(m2.data(), m2.size()));
    std::span<const std::byte> ct(r2.data() + 4, r2.size() - 4);
    std::vector<std::byte> out;
    CHECK(!b.open(ct, out)); // B still at counter 0 => nonce mismatch
}

TEST("mismatched PSK fails authentication") {
    crypto::secure_session a, b;
    CHECK(!handshake_pair("the-right-key", "the-WRONG-key", "g", a, b));
}

TEST("mismatched group name fails authentication") {
    // Same PSK, different group string mixed into the transcript.
    auto k  = crypto::derive_group_key("same-psk");
    auto ea = crypto::gen_ephemeral();
    auto eb = crypto::gen_ephemeral();
    crypto::handshake_result ra, rb;
    CHECK(crypto::do_handshake(ea, eb.pk, k, "group-one", ra));
    CHECK(crypto::do_handshake(eb, ea.pk, k, "group-two", rb));
    CHECK(!crypto::verify_tag(ra.expectedPeerTag, rb.myConfirmTag));
}

TEST("discovery MAC verifies and rejects tampering / wrong key") {
    auto group = crypto::derive_group_key("disco-psk");
    auto dkey  = crypto::derive_discovery_key(group);
    auto other = crypto::derive_discovery_key(crypto::derive_group_key("different"));

    auto datagram = bytes_of("M M C A announce payload bytes");
    std::span<const std::byte> msg(datagram.data(), datagram.size());
    auto tag = crypto::discovery_tag(msg, dkey);

    CHECK(crypto::discovery_verify(msg, std::span<const std::byte>(tag.data(), tag.size()), dkey));
    // wrong key
    CHECK(!crypto::discovery_verify(msg, std::span<const std::byte>(tag.data(), tag.size()), other));
    // tampered message
    datagram[0] ^= std::byte{0xFF};
    CHECK(!crypto::discovery_verify(std::span<const std::byte>(datagram.data(), datagram.size()),
                                    std::span<const std::byte>(tag.data(), tag.size()), dkey));
}

TEST("identity: seed is deterministic, keypair derives stable id") {
    crypto::seed32 seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<std::byte>(i + 1);
    auto a = crypto::identity_from_seed(seed);
    auto b = crypto::identity_from_seed(seed);
    CHECK(a.pk == b.pk);
    CHECK(crypto::id_from_identity(a.pk) == crypto::id_from_identity(b.pk));
    // Different seed => different id.
    seed[0] = std::byte{0xFF};
    auto c = crypto::identity_from_seed(seed);
    CHECK(!(crypto::id_from_identity(c.pk) == crypto::id_from_identity(a.pk)));
    // seed_of round-trips back to the same seed (and thus same keypair).
    auto kp   = crypto::gen_identity();
    auto s2   = crypto::seed_of(kp);
    auto kp2  = crypto::identity_from_seed(s2);
    CHECK(kp.pk == kp2.pk);
}

TEST("identity: sign / verify, with tamper and wrong-key rejection") {
    auto kp    = crypto::gen_identity();
    auto other = crypto::gen_identity();
    auto msg   = bytes_of("bind this transcript");
    std::span<const std::byte> m(msg.data(), msg.size());
    auto sig = crypto::sign(m, kp.sk);

    CHECK(crypto::verify_sig(m, sig, kp.pk));
    // wrong public key
    CHECK(!crypto::verify_sig(m, sig, other.pk));
    // tampered message
    auto msg2 = bytes_of("bind this transcript!");
    CHECK(!crypto::verify_sig(std::span<const std::byte>(msg2.data(), msg2.size()), sig, kp.pk));
    // tampered signature
    sig[0] ^= std::byte{0x01};
    CHECK(!crypto::verify_sig(m, sig, kp.pk));
}

int main() { return mm::test::run(); }
