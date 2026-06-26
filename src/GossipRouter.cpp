#include "GossipRouter.hpp"

#include <cstring>
#include <random>

namespace mm {

GossipRouter::GossipRouter(const MeshConfig& cfg, EventLoop& loop, const PeerId& self)
    : cfg_(cfg), loop_(loop), self_(self) {
    // Seed the counter randomly so message ids don't collide with those minted
    // before a restart (best-effort; ids are also namespaced by nodeId).
    std::random_device rd;
    counter_ = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
}

void GossipRouter::start() {
    // Sweep at a quarter of the dedup TTL so entries don't linger far past it.
    auto period = cfg_.dedupTtl / 4;
    if (period.count() <= 0) period = std::chrono::milliseconds(1000);
    loop_.addTimer(period, [this, period] {
        evictSweep();
        // re-arm
        loop_.addTimer(period, [this] { evictSweep(); });
    });
}

MessageId GossipRouter::nextId() {
    MessageId id;
    std::memcpy(id.bytes.data(), self_.bytes.data(), 8); // low 8 bytes: originator
    std::uint64_t c = ++counter_;
    for (int i = 0; i < 8; ++i) {
        id.bytes[8 + i] = static_cast<std::byte>((c >> ((7 - i) * 8)) & 0xFF);
    }
    return id;
}

bool GossipRouter::seen(const MessageId& id) {
    if (dedup_.find(id) != dedup_.end()) return true;
    dedup_.insert(id);
    order_.push_back({id, EventLoop::Clock::now()});
    // Hard size cap (storm protection): evict oldest.
    while (order_.size() > cfg_.maxDedupEntries) {
        dedup_.erase(order_.front().id);
        order_.pop_front();
    }
    return false;
}

void GossipRouter::evictSweep() {
    auto cutoff = EventLoop::Clock::now() - cfg_.dedupTtl;
    while (!order_.empty() && order_.front().at < cutoff) {
        dedup_.erase(order_.front().id);
        order_.pop_front();
    }
}

void GossipRouter::onData(const DataView& view, int inboundFd, Forwarder& fwd) {
    if (seen(view.msgId)) return; // duplicate => drop entirely (no deliver, no forward)

    const bool isBroadcast = view.dst.isZero();

    if (isBroadcast) {
        if (!(view.src == self_)) fwd.deliverLocal(view.src, view.payload);
        if (view.ttl > 1) {
            auto frame = encodeData(view.src, view.dst, view.msgId,
                                    static_cast<std::uint8_t>(view.ttl - 1), view.payload);
            fwd.forwardExcept(std::span<const std::byte>(frame.data(), frame.size()), inboundFd);
        }
        return;
    }

    // Targeted.
    if (view.dst == self_) {
        fwd.deliverLocal(view.src, view.payload); // arrived; do not forward
        return;
    }
    // Relay only (no local delivery).
    if (view.ttl > 1) {
        auto frame = encodeData(view.src, view.dst, view.msgId,
                                static_cast<std::uint8_t>(view.ttl - 1), view.payload);
        std::span<const std::byte> span(frame.data(), frame.size());
        if (!fwd.forwardTo(view.dst, span)) {
            fwd.forwardExcept(span, inboundFd); // unknown route => diffuse
        }
    }
}

void GossipRouter::originateBroadcast(Bytes payload, Forwarder& fwd) {
    MessageId id = nextId();
    seen(id); // suppress our own echo coming back
    auto frame = encodeData(self_, PeerId{}, id, cfg_.initialTtl, payload);
    fwd.forwardExcept(std::span<const std::byte>(frame.data(), frame.size()), -1);
}

void GossipRouter::originateTargeted(const PeerId& dst, Bytes payload, Forwarder& fwd) {
    if (dst == self_) {           // sending to ourselves: deliver directly
        fwd.deliverLocal(self_, payload);
        return;
    }
    MessageId id = nextId();
    seen(id);
    auto frame = encodeData(self_, dst, id, cfg_.initialTtl, payload);
    std::span<const std::byte> span(frame.data(), frame.size());
    if (!fwd.forwardTo(dst, span)) {
        fwd.forwardExcept(span, -1);
    }
}

} // namespace mm
