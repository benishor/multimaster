#include "multimaster/PeerId.hpp"

#include <cstring>
#include <random>

namespace mm {

namespace {
constexpr char kHex[] = "0123456789abcdef";

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::string PeerId::toString() const {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::byte b : bytes) {
        auto v = static_cast<unsigned>(b);
        out.push_back(kHex[v >> 4]);
        out.push_back(kHex[v & 0x0F]);
    }
    return out;
}

std::optional<PeerId> PeerId::fromString(std::string_view sv) {
    // Accept dashes as cosmetic separators; require exactly 32 hex digits.
    PeerId id;
    std::size_t nibble = 0;
    for (char c : sv) {
        if (c == '-') continue;
        int v = hexVal(c);
        if (v < 0) return std::nullopt;
        if (nibble >= 32) return std::nullopt;
        auto& dst = id.bytes[nibble / 2];
        if (nibble % 2 == 0) {
            dst = static_cast<std::byte>(v << 4);
        } else {
            dst = static_cast<std::byte>(static_cast<unsigned>(dst) | v);
        }
        ++nibble;
    }
    if (nibble != 32) return std::nullopt;
    return id;
}

PeerId PeerId::generate() {
    PeerId id;
    std::random_device rd;
    // Fill 16 bytes from a 32-bit entropy source, 4 bytes at a time.
    for (std::size_t i = 0; i < id.bytes.size(); i += 4) {
        std::uint32_t r = rd();
        id.bytes[i + 0] = static_cast<std::byte>(r & 0xFF);
        id.bytes[i + 1] = static_cast<std::byte>((r >> 8) & 0xFF);
        id.bytes[i + 2] = static_cast<std::byte>((r >> 16) & 0xFF);
        id.bytes[i + 3] = static_cast<std::byte>((r >> 24) & 0xFF);
    }
    return id;
}

bool PeerId::isZero() const {
    for (std::byte b : bytes) {
        if (b != std::byte{0}) return false;
    }
    return true;
}

} // namespace mm

std::size_t std::hash<mm::PeerId>::operator()(const mm::PeerId& id) const noexcept {
    // FNV-1a over the 16 bytes — fast and good enough for hash-table bucketing.
    std::uint64_t h = 1469598103934665603ULL;
    for (std::byte b : id.bytes) {
        h ^= static_cast<std::uint64_t>(b);
        h *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(h);
}
