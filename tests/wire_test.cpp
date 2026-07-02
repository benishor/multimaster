// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "test_harness.hpp"

#include "wire.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace mm;

namespace {
std::vector<std::byte> bytes_of(const std::string& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}
peer_id id_from(unsigned seed) {
    peer_id id;
    for (auto& b : id.bytes) b = static_cast<std::byte>(seed++);
    return id;
}
} // namespace

TEST("announce round-trip") {
    announce a;
    a.protocolVersion = 1;
    a.flags           = 0;
    a.tcpListenPort   = 45123;
    a.nodeId          = id_from(7);
    a.groupName       = "team-alpha";

    auto enc = encode_announce(a);
    auto dec = decode_announce(std::span<const std::byte>(enc.data(), enc.size()));
    CHECK(dec.has_value());
    CHECK_EQ(dec->protocolVersion, a.protocolVersion);
    CHECK_EQ(dec->tcpListenPort, a.tcpListenPort);
    CHECK(dec->nodeId == a.nodeId);
    CHECK_EQ(dec->groupName, a.groupName);
}

TEST("announce rejects bad magic") {
    std::vector<std::byte> junk(32, std::byte{0xAB});
    auto dec = decode_announce(std::span<const std::byte>(junk.data(), junk.size()));
    CHECK(!dec.has_value());
}

TEST("hello round-trip via frame decode") {
    hello h;
    h.nodeId          = id_from(3);
    h.protocolVersion = 1;
    h.tcpListenPort   = 5000;
    h.groupName       = "g";
    h.nonce           = 0xDEADBEEFCAFEULL;

    auto frame = encode_hello(frame_type::Hello, h);
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK_EQ(consumed, frame.size());
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(frame_type::Hello));
    CHECK(out.hello_msg.nodeId == h.nodeId);
    CHECK_EQ(out.hello_msg.tcpListenPort, h.tcpListenPort);
    CHECK_EQ(out.hello_msg.groupName, h.groupName);
    CHECK_EQ(out.hello_msg.nonce, h.nonce);
}

TEST("secure hello carries ephemeral pubkey") {
    hello h;
    h.nodeId          = id_from(3);
    h.protocolVersion = 1;
    h.tcpListenPort   = 5000;
    h.groupName       = "g";
    h.nonce           = 0x1234ULL;
    h.secure          = true;
    for (std::size_t i = 0; i < h.ephPubKey.size(); ++i)
        h.ephPubKey[i] = static_cast<std::byte>(i + 100);

    auto frame = encode_hello(frame_type::HelloAck, h);
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(frame_type::HelloAck));
    CHECK(out.hello_msg.secure);
    CHECK(out.hello_msg.ephPubKey == h.ephPubKey);
}

TEST("auth confirm round-trip") {
    std::array<std::byte, 32> tag{};
    for (std::size_t i = 0; i < tag.size(); ++i) tag[i] = static_cast<std::byte>(i * 7 + 1);
    auto frame = encode_auth_confirm(tag);
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(frame_type::AuthConfirm));
    CHECK(out.auth_tag == tag);
    CHECK(!out.has_auth_sig);
}

TEST("auth confirm with identity signature round-trip") {
    std::array<std::byte, 32> tag{};
    std::array<std::byte, 64> sig{};
    for (std::size_t i = 0; i < tag.size(); ++i) tag[i] = static_cast<std::byte>(i + 3);
    for (std::size_t i = 0; i < sig.size(); ++i) sig[i] = static_cast<std::byte>(i * 5 + 2);
    auto frame = encode_auth_confirm(tag, true, sig);
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK(out.auth_tag == tag);
    CHECK(out.has_auth_sig);
    CHECK(out.auth_sig == sig);
}

TEST("secure hello with identity carries id key and name") {
    hello h;
    h.nodeId          = id_from(4);
    h.protocolVersion = 1;
    h.tcpListenPort   = 6000;
    h.groupName       = "g";
    h.secure          = true;
    h.hasIdentity     = true;
    for (std::size_t i = 0; i < h.ephPubKey.size(); ++i) h.ephPubKey[i] = static_cast<std::byte>(i);
    for (std::size_t i = 0; i < h.idPubKey.size(); ++i)
        h.idPubKey[i] = static_cast<std::byte>(200 - i);
    h.nodeName = "gateway-eu";

    auto frame = encode_hello(frame_type::Hello, h);
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK(out.hello_msg.secure);
    CHECK(out.hello_msg.hasIdentity);
    CHECK(out.hello_msg.idPubKey == h.idPubKey);
    CHECK_EQ(out.hello_msg.nodeName, h.nodeName);
}

TEST("identity record round-trip") {
    identity_record r;
    for (std::size_t i = 0; i < r.idPubKey.size(); ++i) r.idPubKey[i] = static_cast<std::byte>(i);
    r.version = 0x0102030405060708ULL;
    r.name    = "sensor-07";
    for (std::size_t i = 0; i < r.signature.size(); ++i) r.signature[i] = static_cast<std::byte>(255 - i);

    auto frame = encode_identity(r);
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(frame_type::IdentityRecord));
    CHECK(out.identity.idPubKey == r.idPubKey);
    CHECK_EQ(out.identity.version, r.version);
    CHECK_EQ(out.identity.name, r.name);
    CHECK(out.identity.signature == r.signature);
}

TEST("data round-trip preserves payload") {
    auto payload = bytes_of("hello mesh");
    message_id mid;
    for (auto& b : mid.bytes) b = std::byte{0x11};
    auto frame = encode_data(id_from(1), peer_id{}, mid, 8,
                            std::span<const std::byte>(payload.data(), payload.size()));
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(frame_type::Data));
    CHECK_EQ(out.data.ttl, 8);
    CHECK(out.data.dst.is_zero());
    CHECK_EQ(out.data.payload.size(), payload.size());
    CHECK(std::memcmp(out.data.payload.data(), payload.data(), payload.size()) == 0);
}

TEST("decode reports NeedMore on partial input") {
    hello h;
    h.nodeId = id_from(9);
    auto frame = encode_hello(frame_type::Hello, h);
    // Feed only the first few bytes.
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), 3),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::NeedMore));

    // First half of the body, still incomplete.
    st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size() - 2),
                        1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::NeedMore));
}

TEST("decode rejects oversized frame") {
    auto payload = bytes_of(std::string(100, 'x'));
    message_id mid{};
    auto frame = encode_data(id_from(1), peer_id{}, mid, 4,
                            std::span<const std::byte>(payload.data(), payload.size()));
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             16, out, consumed); // max far below frame size
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Error));
}

TEST("membership round-trip") {
    membership_record m;
    m.origin    = id_from(5);
    m.version   = 0xA1B2C3D4E5F60718ULL;
    m.neighbors = {id_from(20), id_from(40), id_from(60)};

    auto frame = encode_membership(m);
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(frame_type::Membership));
    CHECK(out.membership.origin == m.origin);
    CHECK_EQ(out.membership.version, m.version);
    CHECK_EQ(out.membership.neighbors.size(), m.neighbors.size());
    CHECK(out.membership.neighbors[0] == m.neighbors[0]);
    CHECK(out.membership.neighbors[2] == m.neighbors[2]);
}

TEST("membership with no neighbors round-trips") {
    membership_record m;
    m.origin  = id_from(1);
    m.version = 7;
    auto frame = encode_membership(m);
    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK(out.membership.neighbors.empty());
    CHECK_EQ(out.membership.version, std::uint64_t{7});
}

TEST("two frames decode sequentially from one buffer") {
    auto hb1 = encode_heartbeat();
    auto hb2 = encode_goodbye();
    std::vector<std::byte> buf;
    buf.insert(buf.end(), hb1.begin(), hb1.end());
    buf.insert(buf.end(), hb2.begin(), hb2.end());

    parsed_frame out;
    std::size_t consumed = 0;
    auto st = try_decode_frame(std::span<const std::byte>(buf.data(), buf.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(decode_status::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(frame_type::Heartbeat));
    CHECK_EQ(consumed, hb1.size());

    auto st2 = try_decode_frame(
        std::span<const std::byte>(buf.data() + consumed, buf.size() - consumed),
        1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st2), static_cast<int>(decode_status::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(frame_type::Goodbye));
}

TEST("peerid hex round-trip") {
    peer_id id = id_from(42);
    auto s = id.to_string();
    CHECK_EQ(s.size(), std::size_t{32});
    auto back = peer_id::from_string(s);
    CHECK(back.has_value());
    CHECK(*back == id);
}

int main() { return mm::test::run(); }
