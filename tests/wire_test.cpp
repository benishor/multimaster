#include "test_harness.hpp"

#include "Wire.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace mm;

namespace {
std::vector<std::byte> bytesOf(const std::string& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}
PeerId idFrom(unsigned seed) {
    PeerId id;
    for (auto& b : id.bytes) b = static_cast<std::byte>(seed++);
    return id;
}
} // namespace

TEST("announce round-trip") {
    Announce a;
    a.protocolVersion = 1;
    a.flags           = 0;
    a.tcpListenPort   = 45123;
    a.nodeId          = idFrom(7);
    a.groupName       = "team-alpha";

    auto enc = encodeAnnounce(a);
    auto dec = decodeAnnounce(std::span<const std::byte>(enc.data(), enc.size()));
    CHECK(dec.has_value());
    CHECK_EQ(dec->protocolVersion, a.protocolVersion);
    CHECK_EQ(dec->tcpListenPort, a.tcpListenPort);
    CHECK(dec->nodeId == a.nodeId);
    CHECK_EQ(dec->groupName, a.groupName);
}

TEST("announce rejects bad magic") {
    std::vector<std::byte> junk(32, std::byte{0xAB});
    auto dec = decodeAnnounce(std::span<const std::byte>(junk.data(), junk.size()));
    CHECK(!dec.has_value());
}

TEST("hello round-trip via frame decode") {
    Hello h;
    h.nodeId          = idFrom(3);
    h.protocolVersion = 1;
    h.tcpListenPort   = 5000;
    h.groupName       = "g";
    h.nonce           = 0xDEADBEEFCAFEULL;

    auto frame = encodeHello(FrameType::Hello, h);
    ParsedFrame out;
    std::size_t consumed = 0;
    auto st = tryDecodeFrame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(DecodeStatus::Ok));
    CHECK_EQ(consumed, frame.size());
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(FrameType::Hello));
    CHECK(out.hello.nodeId == h.nodeId);
    CHECK_EQ(out.hello.tcpListenPort, h.tcpListenPort);
    CHECK_EQ(out.hello.groupName, h.groupName);
    CHECK_EQ(out.hello.nonce, h.nonce);
}

TEST("data round-trip preserves payload") {
    auto payload = bytesOf("hello mesh");
    MessageId mid;
    for (auto& b : mid.bytes) b = std::byte{0x11};
    auto frame = encodeData(idFrom(1), PeerId{}, mid, 8,
                            std::span<const std::byte>(payload.data(), payload.size()));
    ParsedFrame out;
    std::size_t consumed = 0;
    auto st = tryDecodeFrame(std::span<const std::byte>(frame.data(), frame.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(DecodeStatus::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(FrameType::Data));
    CHECK_EQ(out.data.ttl, 8);
    CHECK(out.data.dst.isZero());
    CHECK_EQ(out.data.payload.size(), payload.size());
    CHECK(std::memcmp(out.data.payload.data(), payload.data(), payload.size()) == 0);
}

TEST("decode reports NeedMore on partial input") {
    Hello h;
    h.nodeId = idFrom(9);
    auto frame = encodeHello(FrameType::Hello, h);
    // Feed only the first few bytes.
    ParsedFrame out;
    std::size_t consumed = 0;
    auto st = tryDecodeFrame(std::span<const std::byte>(frame.data(), 3),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(DecodeStatus::NeedMore));

    // First half of the body, still incomplete.
    st = tryDecodeFrame(std::span<const std::byte>(frame.data(), frame.size() - 2),
                        1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(DecodeStatus::NeedMore));
}

TEST("decode rejects oversized frame") {
    auto payload = bytesOf(std::string(100, 'x'));
    MessageId mid{};
    auto frame = encodeData(idFrom(1), PeerId{}, mid, 4,
                            std::span<const std::byte>(payload.data(), payload.size()));
    ParsedFrame out;
    std::size_t consumed = 0;
    auto st = tryDecodeFrame(std::span<const std::byte>(frame.data(), frame.size()),
                             16, out, consumed); // max far below frame size
    CHECK_EQ(static_cast<int>(st), static_cast<int>(DecodeStatus::Error));
}

TEST("two frames decode sequentially from one buffer") {
    auto hb1 = encodeHeartbeat();
    auto hb2 = encodeGoodbye();
    std::vector<std::byte> buf;
    buf.insert(buf.end(), hb1.begin(), hb1.end());
    buf.insert(buf.end(), hb2.begin(), hb2.end());

    ParsedFrame out;
    std::size_t consumed = 0;
    auto st = tryDecodeFrame(std::span<const std::byte>(buf.data(), buf.size()),
                             1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(DecodeStatus::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(FrameType::Heartbeat));
    CHECK_EQ(consumed, hb1.size());

    auto st2 = tryDecodeFrame(
        std::span<const std::byte>(buf.data() + consumed, buf.size() - consumed),
        1 << 20, out, consumed);
    CHECK_EQ(static_cast<int>(st2), static_cast<int>(DecodeStatus::Ok));
    CHECK_EQ(static_cast<int>(out.type), static_cast<int>(FrameType::Goodbye));
}

TEST("peerid hex round-trip") {
    PeerId id = idFrom(42);
    auto s = id.toString();
    CHECK_EQ(s.size(), std::size_t{32});
    auto back = PeerId::fromString(s);
    CHECK(back.has_value());
    CHECK(*back == id);
}

int main() { return mm::test::run(); }
