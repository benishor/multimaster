#include "MeshImpl.hpp"

#include <utility>

namespace mm {

MeshImpl::MeshImpl(MeshConfig cfg)
    : cfg_(std::move(cfg)),
      connectedSnap_(std::make_shared<std::vector<PeerId>>()),
      knownSnap_(std::make_shared<std::vector<PeerId>>()) {
    self_.nodeId          = cfg_.nodeId;
    self_.groupName       = cfg_.groupName;
    self_.protocolVersion = cfg_.protocolVersion;
    self_.listenPort      = cfg_.listenPort; // refined after bind
}

MeshImpl::~MeshImpl() { stop(); }

void MeshImpl::setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

void MeshImpl::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return; // already started

    peers_ = std::make_unique<PeerManager>(loop_, cfg_, self_, *this);

    // Bind the TCP listener first so we can advertise the resolved port.
    listener_ = std::make_unique<Listener>(loop_, [this](Socket s, const sockaddr_in& a) {
        peers_->acceptConnection(std::move(s), a);
    });
    listener_->start(cfg_.bindAddr, cfg_.listenPort); // throws on failure
    self_.listenPort = listener_->boundPort();
    listenPort_.store(self_.listenPort);

    discovery_ = std::make_unique<Discovery>(
        loop_, cfg_, self_,
        [this](const Announce& a, const sockaddr_in& src) { peers_->onDiscovered(a, src); },
        [this](const Error& e) { error(e); });

    loop_.setWakeHandler([this] { drainMailbox(); });

    // All registration/timers happen before the loop runs (no concurrency yet).
    peers_->start();
    discovery_->start();

    running_.store(true);
    ioThread_ = std::thread([this] {
        ioThreadId_ = std::this_thread::get_id();
        loop_.run();
    });
}

void MeshImpl::stop() {
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
    post(Command{Command::Type::Stop, {}, {}});

    if (std::this_thread::get_id() == ioThreadId_) {
        // Called from within a callback: cannot self-join. The thread will exit
        // run() and be joined later (next stop() or destructor).
        return;
    }
    if (ioThread_.joinable()) ioThread_.join();
}

void MeshImpl::post(Command&& c) {
    {
        std::lock_guard lk(mailboxMu_);
        mailbox_.push_back(std::move(c));
    }
    loop_.wakeup();
}

void MeshImpl::drainMailbox() {
    std::deque<Command> cmds;
    {
        std::lock_guard lk(mailboxMu_);
        cmds.swap(mailbox_);
    }
    bool stopRequested = false;
    for (auto& c : cmds) {
        switch (c.type) {
        case Command::Type::Broadcast:
            peers_->originateBroadcast(Bytes(c.payload.data(), c.payload.size()));
            break;
        case Command::Type::Targeted:
            peers_->originateTargeted(c.dst, Bytes(c.payload.data(), c.payload.size()));
            break;
        case Command::Type::Stop:
            stopRequested = true;
            break;
        }
    }
    if (stopRequested) {
        peers_->shutdown();
        loop_.stop();
    }
}

void MeshImpl::broadcast(Bytes data) {
    if (!running_.load()) return;
    Command c{Command::Type::Broadcast, {}, {}};
    c.payload.assign(data.begin(), data.end());
    post(std::move(c));
}

void MeshImpl::send(const PeerId& dst, Bytes data) {
    if (!running_.load()) return;
    Command c{Command::Type::Targeted, dst, {}};
    c.payload.assign(data.begin(), data.end());
    post(std::move(c));
}

std::vector<PeerId> MeshImpl::connectedPeers() const {
    std::shared_ptr<const std::vector<PeerId>> snap;
    {
        std::lock_guard lk(snapMu_);
        snap = connectedSnap_;
    }
    return *snap;
}

std::vector<PeerId> MeshImpl::knownPeers() const {
    std::shared_ptr<const std::vector<PeerId>> snap;
    {
        std::lock_guard lk(snapMu_);
        snap = knownSnap_;
    }
    return *snap;
}

// --- PeerManagerDelegate (all invoked on the IO thread) ---------------------

void MeshImpl::peerDiscovered(const PeerId& id) {
    if (cb_.onPeerDiscovered) cb_.onPeerDiscovered(id);
}
void MeshImpl::peerConnected(const PeerId& id) {
    if (cb_.onPeerConnected) cb_.onPeerConnected(id);
}
void MeshImpl::peerDisconnected(const PeerId& id) {
    if (cb_.onPeerDisconnected) cb_.onPeerDisconnected(id);
}
void MeshImpl::peerLost(const PeerId& id) {
    if (cb_.onPeerLost) cb_.onPeerLost(id);
}
void MeshImpl::messageReceived(const PeerId& from, Bytes payload) {
    if (cb_.onMessage) cb_.onMessage(from, payload);
}
void MeshImpl::error(const Error& e) {
    if (cb_.onError) cb_.onError(e);
}
void MeshImpl::connectedSnapshot(std::vector<PeerId> v) {
    auto sp = std::make_shared<const std::vector<PeerId>>(std::move(v));
    std::lock_guard lk(snapMu_);
    connectedSnap_ = std::move(sp);
}
void MeshImpl::knownSnapshot(std::vector<PeerId> v) {
    auto sp = std::make_shared<const std::vector<PeerId>>(std::move(v));
    std::lock_guard lk(snapMu_);
    knownSnap_ = std::move(sp);
}

} // namespace mm
