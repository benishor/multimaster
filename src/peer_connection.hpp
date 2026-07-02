// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "buffer.hpp"
#include "crypto.hpp"
#include "event_loop.hpp"
#include "socket.hpp"
#include "wire.hpp"
#include "multimaster/config.hpp"
#include "multimaster/events.hpp"
#include "multimaster/peer_id.hpp"

#include <cstdint>
#include <optional>
#include <span>

#include <netinet/in.h>

namespace mm {

/// Identity this node advertises to peers in its hello. When `secure` is set the
/// derived `groupKey` / `discoveryKey` authenticate and encrypt the transport
/// and discovery announces respectively.
struct local_identity {
    peer_id        nodeId;
    std::string   groupName;
    std::uint8_t  protocolVersion = 1;
    std::uint16_t listenPort      = 0;
    bool          secure          = false;
    crypto::key32 groupKey{};
    crypto::key32 discoveryKey{};

    // --- self-certifying node identity (set when configured) ----------------
    bool                     hasIdentity = false;
    crypto::identity_keypair idKeys{};   // Ed25519; nodeId is derived from idKeys.pk
    std::string              nodeName;    // self-declared nickname (signed, gossiped)
    // Parsed allowlist of admissible peer identity keys. Empty ⇒ accept any
    // valid identity; non-empty ⇒ admit only these keys (per-node revocation).
    std::vector<crypto::id_pubkey> trustedKeys;
};

class peer_connection;

/// Events surfaced by a peer_connection to its owner (peer_manager). All are
/// invoked on the IO thread.
class connection_listener {
public:
    virtual ~connection_listener() = default;

    /// Handshake validated (group/version matched). `inbound` is true if this
    /// node accepted the connection, false if it dialed out.
    virtual void on_peer_handshake(peer_connection& c, const hello& peer) = 0;

    /// A Data frame arrived. `view.payload` is valid only for this call.
    virtual void on_peer_data(peer_connection& c, const data_view& view) = 0;

    /// A Membership (adjacency gossip) frame arrived.
    virtual void on_peer_membership(peer_connection& c, const membership_record& rec) = 0;

    /// An IdentityRecord (signed node-name gossip) frame arrived.
    virtual void on_peer_identity(peer_connection& c, const identity_record& rec) = 0;

    /// The connection finished (peer closed, error, protocol violation, or
    /// Goodbye). The owner must arrange deferred reaping of `c`.
    virtual void on_peer_closed(peer_connection& c) = 0;

    virtual void report_error(const error& e) = 0;
};

/// One TCP connection to a peer. Owns its socket and drives framed, partial
/// reads/writes plus the hello handshake. Registered with the event_loop as an
/// io_handler. Never deletes itself — closure routes through
/// connection_listener::onPeerClosed so the owner can reap after the event batch.
class peer_connection : public io_handler {
public:
    enum class conn_state { ConnectingOut, Handshaking, AwaitingConfirm, Established, Dead };

    /// Construct around an already-connected, accepted socket (inbound).
    peer_connection(event_loop& loop, connection_listener& listener,
                   const mesh_config& cfg, const local_identity& self,
                   socket sock, const sockaddr_in& peerAddr, std::uint64_t nonce);

    /// Construct for an outbound dial; call start_connect() afterwards.
    peer_connection(event_loop& loop, connection_listener& listener,
                   const mesh_config& cfg, const local_identity& self,
                   const sockaddr_in& target, std::uint64_t nonce);

    ~peer_connection() override;

    /// Begin a nonblocking connect (outbound only). Returns false if the socket
    /// or connect() failed outright (the connection is then Dead).
    bool start_connect();

    void on_io_events(std::uint32_t events) override;

    /// Enqueue an already-encoded frame for transmission (gossip forwarding).
    void send_raw(std::span<const std::byte> frame);

    /// Send a Heartbeat if the link is idle (used by the liveness tick).
    void maybe_heartbeat(event_loop::clock::time_point now,
                        std::chrono::milliseconds interval);

    /// Queue a Goodbye then mark for closure.
    void close_gracefully();

    // --- accessors ----------------------------------------------------------
    [[nodiscard]] conn_state                       state() const noexcept { return state_; }
    [[nodiscard]] bool                        inbound() const noexcept { return inbound_; }
    [[nodiscard]] const peer_id&               id() const noexcept { return peerId_; }
    [[nodiscard]] std::uint16_t               peer_listen_port() const noexcept { return peerListenPort_; }
    [[nodiscard]] std::uint64_t               local_nonce() const noexcept { return localNonce_; }
    [[nodiscard]] std::uint64_t               peer_nonce() const noexcept { return peerNonce_; }
    [[nodiscard]] int                         fd() const noexcept { return sock_.get(); }
    [[nodiscard]] const sockaddr_in&          peer_addr() const noexcept { return peerAddr_; }
    [[nodiscard]] event_loop::clock::time_point last_recv() const noexcept { return lastRecv_; }
    [[nodiscard]] event_loop::clock::time_point created_at() const noexcept { return createdAt_; }

private:
    void send_hello();
    void send_auth_confirm();
    void on_connect_complete();
    void do_read();
    void parse_inbound();
    bool parse_plaintext();          // decode plaintext frames straight from in_
    bool drain_records();            // decrypt records from in_ into plain_
    void parse_secure();             // decode plaintext frames from plain_
    bool dispatch(parsed_frame& f, std::size_t consumed, buffer& src); // false => dead
    bool handle_hello(parsed_frame& f, std::size_t consumed, buffer& src);
    bool handle_auth_confirm(parsed_frame& f, std::size_t consumed, buffer& src);
    // Transcript signed/verified for identity: "mm-id-v1" || firstEph || secondEph || name.
    std::vector<std::byte> id_transcript(const std::array<std::byte, 32>& firstEph,
                                        const std::array<std::byte, 32>& secondEph,
                                        const std::string& name) const;
    void flush_outbound();
    void enqueue(std::span<const std::byte> frame); // seal if secure+established, else raw
    void update_epoll();
    void fail(error_category cat, int err, std::string what);
    void mark_dead();

    event_loop&          loop_;
    connection_listener& listener_;
    const mesh_config&   cfg_;
    const local_identity& self_;

    socket      sock_;
    sockaddr_in peerAddr_{};
    bool        inbound_ = false;
    conn_state       state_   = conn_state::Handshaking;

    peer_id        peerId_;
    std::uint16_t peerListenPort_ = 0;
    std::uint64_t localNonce_     = 0;
    std::uint64_t peerNonce_      = 0;

    // --- security (active only when self_.secure) ---------------------------
    bool                              secure_ = false;
    crypto::ephemeral_keypair         ephKeys_;
    std::optional<crypto::secure_session> sess_;
    crypto::tag32                     myConfirmTag_{};
    crypto::tag32                     expectedPeerTag_{};
    bool                              useIdentity_ = false; // both ends presented an identity
    crypto::id_sig                    myIdSig_{};           // our transcript signature to send
    hello                             peerHello_;   // saved for post-auth handshake
    buffer                            plain_;       // decrypted inbound frame bytes

    buffer in_;
    buffer out_;
    bool   wantWrite_ = false; // EPOLLOUT currently armed?

    event_loop::clock::time_point createdAt_;
    event_loop::clock::time_point lastRecv_;
    event_loop::clock::time_point lastSend_;

    bool registered_ = false;
    bool dead_       = false;
};

} // namespace mm
