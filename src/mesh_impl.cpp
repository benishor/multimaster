// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "mesh_impl.hpp"

#include <cctype>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#include <sys/stat.h>

namespace mm {

namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode exactly `n` bytes from 2n hex chars (whitespace trimmed). Returns false
// on any malformed input or length mismatch.
bool decode_hex(std::string_view sv, std::byte* out, std::size_t n) {
    std::size_t begin = 0, end = sv.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(sv[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(sv[end - 1]))) --end;
    sv = sv.substr(begin, end - begin);
    if (sv.size() != n * 2) return false;
    for (std::size_t i = 0; i < n; ++i) {
        int hi = hex_nibble(sv[i * 2]);
        int lo = hex_nibble(sv[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::byte>((hi << 4) | lo);
    }
    return true;
}

std::string to_hex(const std::byte* p, std::size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string        s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        auto v = static_cast<unsigned>(p[i]);
        s.push_back(kHex[v >> 4]);
        s.push_back(kHex[v & 0x0F]);
    }
    return s;
}

} // namespace

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

bool mesh_impl::load_or_create_seed(crypto::seed32& seed) {
    if (!cfg_.identitySeedHex.empty()) {
        if (!decode_hex(cfg_.identitySeedHex, seed.data(), seed.size())) {
            on_error({error_category::Identity, 0, "invalid identitySeedHex", std::nullopt});
            return false;
        }
        return true;
    }
    // Load an existing identity file if present.
    if (std::ifstream in(cfg_.identityFile, std::ios::binary); in) {
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!decode_hex(content, seed.data(), seed.size())) {
            on_error({error_category::Identity, 0, "malformed identity file: " + cfg_.identityFile,
                      std::nullopt});
            return false;
        }
        return true;
    }
    // Otherwise generate one and persist it (owner-only).
    seed = crypto::seed_of(crypto::gen_identity());
    std::ofstream out(cfg_.identityFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        on_error({error_category::Identity, 0, "cannot write identity file: " + cfg_.identityFile,
                  std::nullopt});
        return false;
    }
    auto hex = to_hex(seed.data(), seed.size());
    out.write(hex.data(), static_cast<std::streamsize>(hex.size()));
    out.close();
    ::chmod(cfg_.identityFile.c_str(), S_IRUSR | S_IWUSR); // 0600
    return true;
}

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

    // Self-certifying node identity (complements the PSK). When configured, the
    // node gets a persistent Ed25519 keypair and its nodeId becomes a hash of the
    // public key, so identity is provable and stable across restarts.
    const bool identityConfigured = !cfg_.identityFile.empty() || !cfg_.identitySeedHex.empty();
    if (identityConfigured) {
        if (cfg_.psk.empty()) {
            on_error({error_category::Identity, 0,
                      "node identity requires a PSK (a secured mesh)", std::nullopt});
            return;
        }
        crypto::seed32 seed{};
        if (!load_or_create_seed(seed)) return; // reports its own error

        self_.idKeys      = crypto::identity_from_seed(seed);
        self_.hasIdentity = true;
        self_.nodeName    = cfg_.nodeName;

        // Derive the node id from the identity public key (overrides cfg_.nodeId).
        auto idbytes = crypto::id_from_identity(self_.idKeys.pk);
        std::memcpy(cfg_.nodeId.bytes.data(), idbytes.data(), idbytes.size());
        self_.nodeId = cfg_.nodeId;

        // Parse the allowlist (admissible keys) and local labels.
        for (const auto& t : cfg_.trustedKeys) {
            crypto::id_pubkey pk{};
            if (!decode_hex(t.publicKeyHex, pk.data(), pk.size())) {
                on_error({error_category::Identity, 0,
                          "invalid trustedKeys public key: " + t.publicKeyHex, std::nullopt});
                return;
            }
            self_.trustedKeys.push_back(pk);
            if (!t.label.empty()) {
                auto    lid = crypto::id_from_identity(pk);
                peer_id pid;
                std::memcpy(pid.bytes.data(), lid.data(), lid.size());
                localLabels_[pid] = t.label;
            }
        }
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

std::string mesh_impl::identity_public_key() const {
    if (!self_.hasIdentity) return {};
    return to_hex(self_.idKeys.pk.data(), self_.idKeys.pk.size());
}

std::string mesh_impl::node_name(const peer_id& id) const {
    // Local label (operator-assigned) wins; it is immutable after start().
    if (auto it = localLabels_.find(id); it != localLabels_.end()) return it->second;
    std::shared_ptr<const std::unordered_map<peer_id, std::string>> snap;
    {
        std::lock_guard lk(snapMu_);
        snap = namesSnap_;
    }
    if (snap) {
        if (auto it = snap->find(id); it != snap->end()) return it->second;
    }
    return {};
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
void mesh_impl::names_snapshot(std::unordered_map<peer_id, std::string> m) {
    auto sp = std::make_shared<const std::unordered_map<peer_id, std::string>>(std::move(m));
    std::lock_guard lk(snapMu_);
    namesSnap_ = std::move(sp);
}

} // namespace mm
