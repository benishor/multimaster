// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "discovery.hpp"
#include "event_loop.hpp"
#include "listener.hpp"
#include "multimaster/config.hpp"
#include "multimaster/events.hpp"
#include "multimaster/peer_id.hpp"
#include "peer_connection.hpp"  // local_identity
#include "peer_manager.hpp"

namespace mm {

/// The full mesh node implementation, hidden behind the mesh pimpl. Owns the
/// IO thread and all subsystems; implements peer_manager_delegate to bridge
/// internal events to the user's callbacks and to maintain thread-safe
/// snapshots of mesh state.
class mesh_impl : public peer_manager_delegate {
 public:
  explicit mesh_impl(mesh_config cfg);
  ~mesh_impl() override;

  void set_callbacks(callbacks cb);
  void start();
  void stop();
  [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

  void broadcast(bytes data);
  void send(const peer_id& dst, bytes data);

  [[nodiscard]] peer_id id() const noexcept { return cfg_.nodeId; }
  [[nodiscard]] std::uint16_t listen_port() const noexcept {
    return listenPort_.load();
  }
  [[nodiscard]] std::vector<peer_id> connected_peers() const;
  [[nodiscard]] std::vector<peer_id> known_peers() const;
  [[nodiscard]] std::vector<peer_id> members() const;
  [[nodiscard]] std::string node_name(const peer_id&) const;
  /// This node's Ed25519 identity public key as hex, or "" if no identity is
  /// configured. Meaningful after start(). Share it so peers can allowlist you.
  [[nodiscard]] std::string identity_public_key() const;

  // peer_manager_delegate
  void peer_discovered(const peer_id&) override;
  void peer_connected(const peer_id&) override;
  void peer_disconnected(const peer_id&) override;
  void peer_lost(const peer_id&) override;
  void member_joined(const peer_id&) override;
  void member_left(const peer_id&) override;
  void message_received(const peer_id& from, bytes payload) override;
  void on_error(const error&) override;
  void connected_snapshot(std::vector<peer_id>) override;
  void known_snapshot(std::vector<peer_id>) override;
  void members_snapshot(std::vector<peer_id>) override;
  void names_snapshot(std::unordered_map<peer_id, std::string>) override;

 private:
  struct command {
    enum class kind { Broadcast, Targeted, Stop } type;
    peer_id dst;
    std::vector<std::byte> payload;
  };

  void post(command&& c);
  void drain_mailbox();
  /// Load the identity seed from identitySeedHex / identityFile, generating and
  /// persisting one if the file does not yet exist. Reports an error and
  /// returns false on any failure.
  bool load_or_create_seed(crypto::seed32& seed);

  mesh_config cfg_;
  local_identity self_;
  callbacks cb_;

  event_loop loop_;
  std::unique_ptr<peer_manager> peers_;
  std::unique_ptr<listener> listener_;
  std::unique_ptr<discovery> discovery_;

  std::thread ioThread_;
  std::thread::id ioThreadId_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
  std::atomic<std::uint16_t> listenPort_{0};

  std::mutex mailboxMu_;
  std::deque<command> mailbox_;

  mutable std::mutex snapMu_;
  std::shared_ptr<const std::vector<peer_id>> connectedSnap_;
  std::shared_ptr<const std::vector<peer_id>> knownSnap_;
  std::shared_ptr<const std::vector<peer_id>> membersSnap_;
  std::shared_ptr<const std::unordered_map<peer_id, std::string>> namesSnap_;

  // Operator-assigned labels (from trustedKeys), immutable after start();
  // override any name a peer advertises.
  std::unordered_map<peer_id, std::string> localLabels_;
};

}  // namespace mm
