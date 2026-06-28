#include "mesh_impl.hpp"

#include <utility>

namespace mm {

mesh_impl::mesh_impl(mesh_config cfg)
    : cfg_(std::move(cfg)),
      connectedSnap_(std::make_shared<std::vector<peer_id>>()),
      knownSnap_(std::make_shared<std::vector<peer_id>>()),
      membersSnap_(std::make_shared<std::vector<peer_id>>()) {
    self_.nodeId          = cfg_.nodeId;
    self_.groupName       = cfg_.groupName;
    self_.protocolVersion = cfg_.protocolVersion;
    self_.listenPort      = cfg_.listenPort; // refined after bind
}

mesh_impl::~mesh_impl() { stop(); }

void mesh_impl::set_callbacks(callbacks cb) { cb_ = std::move(cb); }

void mesh_impl::start() {
    // A non-empty PSK requests a secured mesh: derive the keys up front and
    // refuse to run if the library was built without crypto support.
    if (!cfg_.psk.empty()) {
        if (!crypto::init()) {
            on_error({error_category::Crypto, 0,
                      "PSK configured but library built without libsodium support", std::nullopt});
            return;
        }
        self_.secure       = true;
        self_.groupKey     = crypto::derive_group_key(cfg_.psk);
        self_.discoveryKey = crypto::derive_discovery_key(self_.groupKey);
    }

    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return; // already started

    peers_ = std::make_unique<peer_manager>(loop_, cfg_, self_, *this);

    // Bind the TCP listener first so we can advertise the resolved port.
    listener_ = std::make_unique<listener>(loop_, [this](socket s, const sockaddr_in& a) {
        peers_->accept_connection(std::move(s), a);
    });
    listener_->start(cfg_.bindAddr, cfg_.listenPort); // throws on failure
    self_.listenPort = listener_->bound_port();
    listenPort_.store(self_.listenPort);

    discovery_ = std::make_unique<discovery>(
        loop_, cfg_, self_,
        [this](const announce& a, const sockaddr_in& src) { peers_->on_discovered(a, src); },
        [this](const error& e) { on_error(e); });

    loop_.set_wake_handler([this] { drain_mailbox(); });

    // All registration/timers happen before the loop runs (no concurrency yet).
    peers_->start();
    discovery_->start();

    running_.store(true);
    ioThread_ = std::thread([this] {
        ioThreadId_ = std::this_thread::get_id();
        loop_.run();
    });
}

void mesh_impl::stop() {
    if (!started_.load()) return;
    if (!running_.exchange(false)) {
        // Not running but thread may still be joinable (e.g. stopped from a
        // callback). Join if we're not on the IO thread.
        if (ioThread_.joinable() && std::this_thread::get_id() != ioThreadId_) {
            ioThread_.join();
        }
        return;
    }

    // Ask the IO thread to gracefully shut the mesh down and exit run().
    post(command{command::kind::Stop, {}, {}});

    if (std::this_thread::get_id() == ioThreadId_) {
        // Called from within a callback: cannot self-join. The thread will exit
        // run() and be joined later (next stop() or destructor).
        return;
    }
    if (ioThread_.joinable()) ioThread_.join();
}

void mesh_impl::post(command&& c) {
    {
        std::lock_guard lk(mailboxMu_);
        mailbox_.push_back(std::move(c));
    }
    loop_.wakeup();
}

void mesh_impl::drain_mailbox() {
    std::deque<command> cmds;
    {
        std::lock_guard lk(mailboxMu_);
        cmds.swap(mailbox_);
    }
    bool stopRequested = false;
    for (auto& c : cmds) {
        switch (c.type) {
        case command::kind::Broadcast:
            peers_->originate_broadcast(bytes(c.payload.data(), c.payload.size()));
            break;
        case command::kind::Targeted:
            peers_->originate_targeted(c.dst, bytes(c.payload.data(), c.payload.size()));
            break;
        case command::kind::Stop:
            stopRequested = true;
            break;
        }
    }
    if (stopRequested) {
        peers_->shutdown();
        loop_.stop();
    }
}

void mesh_impl::broadcast(bytes data) {
    if (!running_.load()) return;
    command c{command::kind::Broadcast, {}, {}};
    c.payload.assign(data.begin(), data.end());
    post(std::move(c));
}

void mesh_impl::send(const peer_id& dst, bytes data) {
    if (!running_.load()) return;
    command c{command::kind::Targeted, dst, {}};
    c.payload.assign(data.begin(), data.end());
    post(std::move(c));
}

std::vector<peer_id> mesh_impl::connected_peers() const {
    std::shared_ptr<const std::vector<peer_id>> snap;
    {
        std::lock_guard lk(snapMu_);
        snap = connectedSnap_;
    }
    return *snap;
}

std::vector<peer_id> mesh_impl::known_peers() const {
    std::shared_ptr<const std::vector<peer_id>> snap;
    {
        std::lock_guard lk(snapMu_);
        snap = knownSnap_;
    }
    return *snap;
}

std::vector<peer_id> mesh_impl::members() const {
    std::shared_ptr<const std::vector<peer_id>> snap;
    {
        std::lock_guard lk(snapMu_);
        snap = membersSnap_;
    }
    return *snap;
}

// --- peer_manager_delegate (all invoked on the IO thread) ---------------------

void mesh_impl::peer_discovered(const peer_id& id) {
    if (cb_.onPeerDiscovered) cb_.onPeerDiscovered(id);
}
void mesh_impl::peer_connected(const peer_id& id) {
    if (cb_.onPeerConnected) cb_.onPeerConnected(id);
}
void mesh_impl::peer_disconnected(const peer_id& id) {
    if (cb_.onPeerDisconnected) cb_.onPeerDisconnected(id);
}
void mesh_impl::peer_lost(const peer_id& id) {
    if (cb_.onPeerLost) cb_.onPeerLost(id);
}
void mesh_impl::member_joined(const peer_id& id) {
    if (cb_.onMemberJoined) cb_.onMemberJoined(id);
}
void mesh_impl::member_left(const peer_id& id) {
    if (cb_.onMemberLeft) cb_.onMemberLeft(id);
}
void mesh_impl::message_received(const peer_id& from, bytes payload) {
    if (cb_.onMessage) cb_.onMessage(from, payload);
}
void mesh_impl::on_error(const error& e) {
    if (cb_.onError) cb_.onError(e);
}
void mesh_impl::connected_snapshot(std::vector<peer_id> v) {
    auto sp = std::make_shared<const std::vector<peer_id>>(std::move(v));
    std::lock_guard lk(snapMu_);
    connectedSnap_ = std::move(sp);
}
void mesh_impl::known_snapshot(std::vector<peer_id> v) {
    auto sp = std::make_shared<const std::vector<peer_id>>(std::move(v));
    std::lock_guard lk(snapMu_);
    knownSnap_ = std::move(sp);
}
void mesh_impl::members_snapshot(std::vector<peer_id> v) {
    auto sp = std::make_shared<const std::vector<peer_id>>(std::move(v));
    std::lock_guard lk(snapMu_);
    membersSnap_ = std::move(sp);
}

} // namespace mm
