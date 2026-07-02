// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "wire.hpp"

#include <cstring>

namespace mm {

namespace {

// --- little serialization helpers (all integers big-endian / network order) -

void put_u8(std::vector<std::byte>& b, std::uint8_t v) {
  b.push_back(static_cast<std::byte>(v));
}
void put_u16(std::vector<std::byte>& b, std::uint16_t v) {
  b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
  b.push_back(static_cast<std::byte>(v & 0xFF));
}
void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
  b.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
  b.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
  b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
  b.push_back(static_cast<std::byte>(v & 0xFF));
}
void put_u64(std::vector<std::byte>& b, std::uint64_t v) {
  for (int i = 7; i >= 0; --i)
    b.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
}
void put_bytes(std::vector<std::byte>& b, const std::byte* p, std::size_t n) {
  b.insert(b.end(), p, p + n);
}
void put_id(std::vector<std::byte>& b, const peer_id& id) {
  put_bytes(b, id.bytes.data(), id.bytes.size());
}
void put_msg_id(std::vector<std::byte>& b, const message_id& id) {
  put_bytes(b, id.bytes.data(), id.bytes.size());
}
void put_str(std::vector<std::byte>& b, const std::string& s) {
  put_u8(b, static_cast<std::uint8_t>(s.size() > 255 ? 255 : s.size()));
  std::size_t n = s.size() > 255 ? 255 : s.size();
  b.insert(b.end(), reinterpret_cast<const std::byte*>(s.data()),
           reinterpret_cast<const std::byte*>(s.data()) + n);
}

/// Cursor-based reader with bounds checking. ok() == false after any overrun.
struct reader {
  const std::byte* p;
  std::size_t remaining;
  bool bad = false;

  explicit reader(std::span<const std::byte> s)
      : p(s.data()), remaining(s.size()) {}

  bool ok() const { return !bad; }

  std::uint8_t u8() {
    if (remaining < 1) {
      bad = true;
      return 0;
    }
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
  void id(peer_id& out) {
    if (remaining < out.bytes.size()) {
      bad = true;
      return;
    }
    std::memcpy(out.bytes.data(), p, out.bytes.size());
    p += out.bytes.size();
    remaining -= out.bytes.size();
  }
  void msg_id(message_id& out) {
    if (remaining < out.bytes.size()) {
      bad = true;
      return;
    }
    std::memcpy(out.bytes.data(), p, out.bytes.size());
    p += out.bytes.size();
    remaining -= out.bytes.size();
  }
  std::string str() {
    std::uint8_t n = u8();
    if (bad || remaining < n) {
      bad = true;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(p), n);
    p += n;
    remaining -= n;
    return s;
  }
  std::span<const std::byte> take(std::size_t n) {
    if (remaining < n) {
      bad = true;
      return {};
    }
    std::span<const std::byte> s(p, n);
    p += n;
    remaining -= n;
    return s;
  }
};

/// Overwrite a 4-byte big-endian length placeholder at offset 0.
void patch_length(std::vector<std::byte>& b) {
  std::uint32_t len = static_cast<std::uint32_t>(b.size() - 4);
  b[0] = static_cast<std::byte>((len >> 24) & 0xFF);
  b[1] = static_cast<std::byte>((len >> 16) & 0xFF);
  b[2] = static_cast<std::byte>((len >> 8) & 0xFF);
  b[3] = static_cast<std::byte>(len & 0xFF);
}

}  // namespace

// ---------------------------------------------------------------------------
// announce
// ---------------------------------------------------------------------------

std::vector<std::byte> encode_announce(const announce& a) {
  std::vector<std::byte> b;
  put_u32(b, kAnnounceMagic);
  put_u8(b, a.protocolVersion);
  put_u8(b, a.flags);
  put_u16(b, a.tcpListenPort);
  put_id(b, a.nodeId);
  put_str(b, a.groupName);
  return b;
}

std::optional<announce> decode_announce(std::span<const std::byte> in) {
  reader r(in);
  if (r.u32() != kAnnounceMagic) return std::nullopt;
  announce a;
  a.protocolVersion = r.u8();
  a.flags = r.u8();
  a.tcpListenPort = r.u16();
  r.id(a.nodeId);
  a.groupName = r.str();
  if (!r.ok()) return std::nullopt;
  return a;
}

// ---------------------------------------------------------------------------
// TCP frames
// ---------------------------------------------------------------------------

std::vector<std::byte> encode_hello(frame_type helloOrAck, const hello& h) {
  std::vector<std::byte> b;
  put_u32(b, 0);  // length placeholder
  put_u8(b, static_cast<std::uint8_t>(helloOrAck));
  put_id(b, h.nodeId);
  put_u8(b, h.protocolVersion);
  put_u16(b, h.tcpListenPort);
  put_str(b, h.groupName);
  put_u64(b, h.nonce);
  put_u8(b, h.secure ? 1 : 0);
  if (h.secure) put_bytes(b, h.ephPubKey.data(), h.ephPubKey.size());
  put_u8(b, h.hasIdentity ? 1 : 0);
  if (h.hasIdentity) {
    put_bytes(b, h.idPubKey.data(), h.idPubKey.size());
    put_str(b, h.nodeName);
  }
  patch_length(b);
  return b;
}

std::vector<std::byte> encode_auth_confirm(
    const std::array<std::byte, 32>& tag, bool hasSig,
    const std::array<std::byte, 64>& sig) {
  std::vector<std::byte> b;
  put_u32(b, 0);
  put_u8(b, static_cast<std::uint8_t>(frame_type::AuthConfirm));
  put_bytes(b, tag.data(), tag.size());
  put_u8(b, hasSig ? 1 : 0);
  if (hasSig) put_bytes(b, sig.data(), sig.size());
  patch_length(b);
  return b;
}

std::vector<std::byte> encode_heartbeat() {
  std::vector<std::byte> b;
  put_u32(b, 0);
  put_u8(b, static_cast<std::uint8_t>(frame_type::Heartbeat));
  patch_length(b);
  return b;
}

std::vector<std::byte> encode_goodbye() {
  std::vector<std::byte> b;
  put_u32(b, 0);
  put_u8(b, static_cast<std::uint8_t>(frame_type::Goodbye));
  patch_length(b);
  return b;
}

std::vector<std::byte> encode_data(const peer_id& src, const peer_id& dst,
                                   const message_id& msgId, std::uint8_t ttl,
                                   std::span<const std::byte> payload) {
  std::vector<std::byte> b;
  b.reserve(4 + 1 + 16 + 16 + 16 + 1 + 4 + payload.size());
  put_u32(b, 0);
  put_u8(b, static_cast<std::uint8_t>(frame_type::Data));
  put_id(b, src);
  put_id(b, dst);
  put_msg_id(b, msgId);
  put_u8(b, ttl);
  put_u32(b, static_cast<std::uint32_t>(payload.size()));
  put_bytes(b, payload.data(), payload.size());
  patch_length(b);
  return b;
}

std::vector<std::byte> encode_membership(const membership_record& m) {
  std::vector<std::byte> b;
  b.reserve(4 + 1 + 16 + 8 + 2 + m.neighbors.size() * 16);
  put_u32(b, 0);
  put_u8(b, static_cast<std::uint8_t>(frame_type::Membership));
  put_id(b, m.origin);
  put_u64(b, m.version);
  put_u16(b, static_cast<std::uint16_t>(
                 m.neighbors.size() > 0xFFFF ? 0xFFFF : m.neighbors.size()));
  std::size_t n = m.neighbors.size() > 0xFFFF ? 0xFFFF : m.neighbors.size();
  for (std::size_t i = 0; i < n; ++i) put_id(b, m.neighbors[i]);
  patch_length(b);
  return b;
}

std::vector<std::byte> encode_identity(const identity_record& r) {
  std::vector<std::byte> b;
  b.reserve(4 + 1 + 32 + 8 + 64 + 1 + r.name.size());
  put_u32(b, 0);
  put_u8(b, static_cast<std::uint8_t>(frame_type::IdentityRecord));
  put_bytes(b, r.idPubKey.data(), r.idPubKey.size());
  put_u64(b, r.version);
  put_bytes(b, r.signature.data(), r.signature.size());
  put_str(b, r.name);
  patch_length(b);
  return b;
}

decode_status try_decode_frame(std::span<const std::byte> in,
                               std::size_t maxMessageBytes, parsed_frame& out,
                               std::size_t& consumed) {
  if (in.size() < 4) return decode_status::NeedMore;

  // Peek the length prefix without consuming.
  std::uint32_t len = (static_cast<std::uint32_t>(in[0]) << 24) |
                      (static_cast<std::uint32_t>(in[1]) << 16) |
                      (static_cast<std::uint32_t>(in[2]) << 8) |
                      (static_cast<std::uint32_t>(in[3]));

  if (len < 1) return decode_status::Error;  // must hold at least frameType
  if (len > maxMessageBytes) return decode_status::Error;  // DoS guard
  if (in.size() < 4 + static_cast<std::size_t>(len))
    return decode_status::NeedMore;

  // Body spans [4, 4+len).
  reader r(in.subspan(4, len));
  auto type = static_cast<frame_type>(r.u8());
  out.type = type;

  switch (type) {
    case frame_type::Hello:
    case frame_type::HelloAck: {
      r.id(out.hello_msg.nodeId);
      out.hello_msg.protocolVersion = r.u8();
      out.hello_msg.tcpListenPort = r.u16();
      out.hello_msg.groupName = r.str();
      out.hello_msg.nonce = r.u64();
      out.hello_msg.secure = r.u8() != 0;
      if (out.hello_msg.secure) {
        auto pk = r.take(out.hello_msg.ephPubKey.size());
        if (r.ok())
          std::memcpy(out.hello_msg.ephPubKey.data(), pk.data(), pk.size());
      }
      out.hello_msg.hasIdentity = r.u8() != 0;
      if (out.hello_msg.hasIdentity) {
        auto idpk = r.take(out.hello_msg.idPubKey.size());
        if (r.ok())
          std::memcpy(out.hello_msg.idPubKey.data(), idpk.data(), idpk.size());
        out.hello_msg.nodeName = r.str();
      }
      break;
    }
    case frame_type::AuthConfirm: {
      auto tag = r.take(out.auth_tag.size());
      if (r.ok()) std::memcpy(out.auth_tag.data(), tag.data(), tag.size());
      out.has_auth_sig = r.u8() != 0;
      if (out.has_auth_sig) {
        auto sig = r.take(out.auth_sig.size());
        if (r.ok()) std::memcpy(out.auth_sig.data(), sig.data(), sig.size());
      }
      break;
    }
    case frame_type::Heartbeat:
    case frame_type::Goodbye:
      break;
    case frame_type::Data: {
      r.id(out.data.src);
      r.id(out.data.dst);
      r.msg_id(out.data.msgId);
      out.data.ttl = r.u8();
      std::uint32_t plen = r.u32();
      out.data.payload = r.take(plen);
      break;
    }
    case frame_type::Membership: {
      out.membership.neighbors.clear();
      r.id(out.membership.origin);
      out.membership.version = r.u64();
      std::uint16_t count = r.u16();
      for (std::uint16_t i = 0; i < count && r.ok(); ++i) {
        peer_id n;
        r.id(n);
        out.membership.neighbors.push_back(n);
      }
      break;
    }
    case frame_type::IdentityRecord: {
      auto pk = r.take(out.identity.idPubKey.size());
      if (r.ok())
        std::memcpy(out.identity.idPubKey.data(), pk.data(), pk.size());
      out.identity.version = r.u64();
      auto sig = r.take(out.identity.signature.size());
      if (r.ok())
        std::memcpy(out.identity.signature.data(), sig.data(), sig.size());
      out.identity.name = r.str();
      break;
    }
    default:
      return decode_status::Error;  // unknown frame type
  }

  if (!r.ok()) return decode_status::Error;
  consumed = 4 + len;
  return decode_status::Ok;
}

}  // namespace mm

std::size_t std::hash<mm::message_id>::operator()(
    const mm::message_id& id) const noexcept {
  std::uint64_t h = 1469598103934665603ULL;
  for (std::byte b : id.bytes) {
    h ^= static_cast<std::uint64_t>(b);
    h *= 1099511628211ULL;
  }
  return static_cast<std::size_t>(h);
}
