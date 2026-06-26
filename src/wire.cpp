#include "wire.hpp"

#include <cstring>

namespace mm {

namespace {

// --- little serialization helpers (all integers big-endian / network order) -

void putU8(std::vector<std::byte>& b, std::uint8_t v) {
    b.push_back(static_cast<std::byte>(v));
}
void putU16(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::byte>(v & 0xFF));
}
void putU32(std::vector<std::byte>& b, std::uint32_t v) {
    b.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
    b.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::byte>(v & 0xFF));
}
void putU64(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) b.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
}
void putBytes(std::vector<std::byte>& b, const std::byte* p, std::size_t n) {
    b.insert(b.end(), p, p + n);
}
void putId(std::vector<std::byte>& b, const PeerId& id) {
    putBytes(b, id.bytes.data(), id.bytes.size());
}
void putMsgId(std::vector<std::byte>& b, const MessageId& id) {
    putBytes(b, id.bytes.data(), id.bytes.size());
}
void putStr(std::vector<std::byte>& b, const std::string& s) {
    putU8(b, static_cast<std::uint8_t>(s.size() > 255 ? 255 : s.size()));
    std::size_t n = s.size() > 255 ? 255 : s.size();
    b.insert(b.end(), reinterpret_cast<const std::byte*>(s.data()),
             reinterpret_cast<const std::byte*>(s.data()) + n);
}

/// Cursor-based reader with bounds checking. ok() == false after any overrun.
struct Reader {
    const std::byte* p;
    std::size_t      remaining;
    bool             bad = false;

    explicit Reader(std::span<const std::byte> s) : p(s.data()), remaining(s.size()) {}

    bool ok() const { return !bad; }

    std::uint8_t u8() {
        if (remaining < 1) { bad = true; return 0; }
        std::uint8_t v = static_cast<std::uint8_t>(*p++);
        --remaining;
        return v;
    }
    std::uint16_t u16() {
        std::uint16_t hi = u8(), lo = u8();
        return static_cast<std::uint16_t>((hi << 8) | lo);
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | u8();
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | u8();
        return v;
    }
    void id(PeerId& out) {
        if (remaining < out.bytes.size()) { bad = true; return; }
        std::memcpy(out.bytes.data(), p, out.bytes.size());
        p += out.bytes.size();
        remaining -= out.bytes.size();
    }
    void msgId(MessageId& out) {
        if (remaining < out.bytes.size()) { bad = true; return; }
        std::memcpy(out.bytes.data(), p, out.bytes.size());
        p += out.bytes.size();
        remaining -= out.bytes.size();
    }
    std::string str() {
        std::uint8_t n = u8();
        if (bad || remaining < n) { bad = true; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        remaining -= n;
        return s;
    }
    std::span<const std::byte> take(std::size_t n) {
        if (remaining < n) { bad = true; return {}; }
        std::span<const std::byte> s(p, n);
        p += n;
        remaining -= n;
        return s;
    }
};

/// Overwrite a 4-byte big-endian length placeholder at offset 0.
void patchLength(std::vector<std::byte>& b) {
    std::uint32_t len = static_cast<std::uint32_t>(b.size() - 4);
    b[0] = static_cast<std::byte>((len >> 24) & 0xFF);
    b[1] = static_cast<std::byte>((len >> 16) & 0xFF);
    b[2] = static_cast<std::byte>((len >> 8) & 0xFF);
    b[3] = static_cast<std::byte>(len & 0xFF);
}

} // namespace

// ---------------------------------------------------------------------------
// Announce
// ---------------------------------------------------------------------------

std::vector<std::byte> encodeAnnounce(const Announce& a) {
    std::vector<std::byte> b;
    putU32(b, kAnnounceMagic);
    putU8(b, a.protocolVersion);
    putU8(b, a.flags);
    putU16(b, a.tcpListenPort);
    putId(b, a.nodeId);
    putStr(b, a.groupName);
    return b;
}

std::optional<Announce> decodeAnnounce(std::span<const std::byte> in) {
    Reader r(in);
    if (r.u32() != kAnnounceMagic) return std::nullopt;
    Announce a;
    a.protocolVersion = r.u8();
    a.flags           = r.u8();
    a.tcpListenPort   = r.u16();
    r.id(a.nodeId);
    a.groupName = r.str();
    if (!r.ok()) return std::nullopt;
    return a;
}

// ---------------------------------------------------------------------------
// TCP frames
// ---------------------------------------------------------------------------

std::vector<std::byte> encodeHello(FrameType helloOrAck, const Hello& h) {
    std::vector<std::byte> b;
    putU32(b, 0); // length placeholder
    putU8(b, static_cast<std::uint8_t>(helloOrAck));
    putId(b, h.nodeId);
    putU8(b, h.protocolVersion);
    putU16(b, h.tcpListenPort);
    putStr(b, h.groupName);
    putU64(b, h.nonce);
    patchLength(b);
    return b;
}

std::vector<std::byte> encodeHeartbeat() {
    std::vector<std::byte> b;
    putU32(b, 0);
    putU8(b, static_cast<std::uint8_t>(FrameType::Heartbeat));
    patchLength(b);
    return b;
}

std::vector<std::byte> encodeGoodbye() {
    std::vector<std::byte> b;
    putU32(b, 0);
    putU8(b, static_cast<std::uint8_t>(FrameType::Goodbye));
    patchLength(b);
    return b;
}

std::vector<std::byte> encodeData(const PeerId& src, const PeerId& dst,
                                  const MessageId& msgId, std::uint8_t ttl,
                                  std::span<const std::byte> payload) {
    std::vector<std::byte> b;
    b.reserve(4 + 1 + 16 + 16 + 16 + 1 + 4 + payload.size());
    putU32(b, 0);
    putU8(b, static_cast<std::uint8_t>(FrameType::Data));
    putId(b, src);
    putId(b, dst);
    putMsgId(b, msgId);
    putU8(b, ttl);
    putU32(b, static_cast<std::uint32_t>(payload.size()));
    putBytes(b, payload.data(), payload.size());
    patchLength(b);
    return b;
}

DecodeStatus tryDecodeFrame(std::span<const std::byte> in, std::size_t maxMessageBytes,
                            ParsedFrame& out, std::size_t& consumed) {
    if (in.size() < 4) return DecodeStatus::NeedMore;

    // Peek the length prefix without consuming.
    std::uint32_t len = (static_cast<std::uint32_t>(in[0]) << 24) |
                        (static_cast<std::uint32_t>(in[1]) << 16) |
                        (static_cast<std::uint32_t>(in[2]) << 8) |
                        (static_cast<std::uint32_t>(in[3]));

    if (len < 1) return DecodeStatus::Error;               // must hold at least frameType
    if (len > maxMessageBytes) return DecodeStatus::Error;  // DoS guard
    if (in.size() < 4 + static_cast<std::size_t>(len)) return DecodeStatus::NeedMore;

    // Body spans [4, 4+len).
    Reader r(in.subspan(4, len));
    auto type = static_cast<FrameType>(r.u8());
    out.type  = type;

    switch (type) {
    case FrameType::Hello:
    case FrameType::HelloAck: {
        r.id(out.hello.nodeId);
        out.hello.protocolVersion = r.u8();
        out.hello.tcpListenPort   = r.u16();
        out.hello.groupName       = r.str();
        out.hello.nonce           = r.u64();
        break;
    }
    case FrameType::Heartbeat:
    case FrameType::Goodbye:
        break;
    case FrameType::Data: {
        r.id(out.data.src);
        r.id(out.data.dst);
        r.msgId(out.data.msgId);
        out.data.ttl            = r.u8();
        std::uint32_t plen      = r.u32();
        out.data.payload        = r.take(plen);
        break;
    }
    default:
        return DecodeStatus::Error; // unknown frame type
    }

    if (!r.ok()) return DecodeStatus::Error;
    consumed = 4 + len;
    return DecodeStatus::Ok;
}

} // namespace mm

std::size_t std::hash<mm::MessageId>::operator()(const mm::MessageId& id) const noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::byte b : id.bytes) {
        h ^= static_cast<std::uint64_t>(b);
        h *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(h);
}
