#include "gossip_router.hpp"

#include <cstring>
#include <random>

namespace mm {

gossip_router::gossip_router(const mesh_config& cfg, event_loop& loop, const peer_id& self)
    : cfg_(cfg), loop_(loop), self_(self) {
    // Seed the counter randomly so message ids don't collide with those minted
    // before a restart (best-effort; ids are also namespaced by nodeId).
    std::random_device rd;
    counter_ = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
}

void gossip_router::start() {
    // Sweep at a quarter of the dedup TTL so entries don't linger far past it.
    auto period = cfg_.dedupTtl / 4;
    if (period.count() <= 0) period = std::chrono::milliseconds(1000);
    loop_.add_timer(period, [this, period] {
        evict_sweep();
        // re-arm
        loop_.add_timer(period, [this] { evict_sweep(); });
    });
}

message_id gossip_router::next_id() {
    message_id id;
    std::memcpy(id.bytes.data(), self_.bytes.data(), 8); // low 8 bytes: originator
    std::uint64_t c = ++counter_;
    for (int i = 0; i < 8; ++i) {
        id.bytes[8 + i] = static_cast<std::byte>((c >> ((7 - i) * 8)) & 0xFF);
    }
    return id;
}

bool gossip_router::seen(const message_id& id) {
    if (dedup_.find(id) != dedup_.end()) return true;
    dedup_.insert(id);
    order_.push_back({id, event_loop::clock::now()});
    // Hard size cap (storm protection): evict oldest.
    while (order_.size() > cfg_.maxDedupEntries) {
        dedup_.erase(order_.front().id);
        order_.pop_front();
    }
    return false;
}

void gossip_router::evict_sweep() {
    auto cutoff = event_loop::clock::now() - cfg_.dedupTtl;
    while (!order_.empty() && order_.front().at < cutoff) {
        dedup_.erase(order_.front().id);
        order_.pop_front();
    }
}

void gossip_router::on_data(const data_view& view, int inboundFd, forwarder& fwd) {
    if (seen(view.msgId)) return; // duplicate => drop entirely (no deliver, no forward)

    const bool isBroadcast = view.dst.is_zero();

    if (isBroadcast) {
        if (!(view.src == self_)) fwd.deliver_local(view.src, view.payload);
        if (view.ttl > 1) {
            auto frame = encode_data(view.src, view.dst, view.msgId,
                                    static_cast<std::uint8_t>(view.ttl - 1), view.payload);
            fwd.forward_except(std::span<const std::byte>(frame.data(), frame.size()), inboundFd);
        }
        return;
    }

    // Targeted.
    if (view.dst == self_) {
        fwd.deliver_local(view.src, view.payload); // arrived; do not forward
        return;
    }
    // Relay only (no local delivery).
    if (view.ttl > 1) {
        auto frame = encode_data(view.src, view.dst, view.msgId,
                                static_cast<std::uint8_t>(view.ttl - 1), view.payload);
        std::span<const std::byte> span(frame.data(), frame.size());
        if (!fwd.forward_to(view.dst, span)) {
            fwd.forward_except(span, inboundFd); // unknown route => diffuse
        }
    }
}

void gossip_router::originate_broadcast(bytes payload, forwarder& fwd) {
    message_id id = next_id();
    seen(id); // suppress our own echo coming back
    auto frame = encode_data(self_, peer_id{}, id, cfg_.initialTtl, payload);
    fwd.forward_except(std::span<const std::byte>(frame.data(), frame.size()), -1);
}

void gossip_router::originate_targeted(const peer_id& dst, bytes payload, forwarder& fwd) {
    if (dst == self_) {           // sending to ourselves: deliver directly
        fwd.deliver_local(self_, payload);
        return;
    }
    message_id id = next_id();
    seen(id);
    auto frame = encode_data(self_, dst, id, cfg_.initialTtl, payload);
    std::span<const std::byte> span(frame.data(), frame.size());
    if (!fwd.forward_to(dst, span)) {
        fwd.forward_except(span, -1);
    }
}

} // namespace mm
