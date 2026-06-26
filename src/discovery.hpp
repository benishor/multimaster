#pragma once

#include "event_loop.hpp"
#include "peer_connection.hpp" // LocalIdentity
#include "socket.hpp"
#include "wire.hpp"
#include "multimaster/config.hpp"
#include "multimaster/events.hpp"

#include <functional>
#include <random>

#include <netinet/in.h>

namespace mm {

/// Periodically multicasts an Announce datagram and listens for peers' announces
/// on the multicast group. Discovered peers (other than self) are surfaced via
/// the discover callback together with the datagram's source address.
class Discovery : public IoHandler {
public:
    using DiscoverFn = std::function<void(const Announce&, const sockaddr_in& src)>;
    using ErrorFn    = std::function<void(const Error&)>;

    Discovery(EventLoop& loop, const MeshConfig& cfg, const LocalIdentity& self,
              DiscoverFn onDiscover, ErrorFn onError);
    ~Discovery() override;

    /// Set up the multicast socket and begin announcing. Reports (but tolerates)
    /// failures via the error callback rather than throwing, so a node can still
    /// run with seed peers when multicast is unavailable.
    void start();

    void onIoEvents(std::uint32_t events) override;

private:
    void announce();
    void scheduleAnnounce();

    EventLoop&           loop_;
    const MeshConfig&    cfg_;
    const LocalIdentity& self_;
    DiscoverFn           onDiscover_;
    ErrorFn              onError_;

    Socket      sock_;
    sockaddr_in groupAddr_{}; // destination for outbound announces
    bool        usable_ = false;

    std::mt19937_64 rng_;
};

} // namespace mm
