// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

// Thin wrapper around libsodium. This is the ONLY header/translation unit that
// knows about the crypto library, so the rest of the mesh deals only in plain
// std::array / std::span byte types. When the project is built without
// MULTIMASTER_HAVE_CRYPTO every function degrades to a safe failure (init()
// returns false; mesh_impl then refuses to start a secured mesh).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mm::crypto {

using key32 = std::array<std::byte, 32>; // X25519 / AEAD key, BLAKE2b digest
using tag32 = std::array<std::byte, 32>; // handshake confirmation tag
using mac16 = std::array<std::byte, 16>; // discovery datagram MAC

/// Initialize libsodium (idempotent). Returns false when built without crypto
/// support or if the library failed to initialize.
bool init();

/// True if the library was compiled with libsodium support.
bool available();

// --- key derivation ---------------------------------------------------------

/// Derive the 32-byte mesh master key from the user's PSK passphrase.
key32 derive_group_key(const std::string& passphrase);

/// Derive the discovery-MAC key from the master key.
key32 derive_discovery_key(const key32& groupKey);

// --- discovery authentication ----------------------------------------------

/// Keyed MAC (16 bytes) over a discovery datagram body.
mac16 discovery_tag(std::span<const std::byte> msg, const key32& discoveryKey);

/// Constant-time verify of a discovery MAC (`tag` must be 16 bytes).
bool discovery_verify(std::span<const std::byte> msg, std::span<const std::byte> tag,
                      const key32& discoveryKey);

// --- handshake --------------------------------------------------------------

struct ephemeral_keypair {
    key32 pk{};
    key32 sk{};
};

/// Generate a fresh per-connection X25519 ephemeral keypair.
ephemeral_keypair gen_ephemeral();

/// An authenticated, encrypted transport for one connection. Each direction has
/// its own key and a monotonic record counter used as the AEAD nonce; TCP's
/// ordered delivery keeps the two ends' counters in lockstep.
class secure_session {
public:
    /// Wrap one plaintext frame into a length-prefixed encrypted record:
    /// `[u32 ciphertextLen][ciphertext || 16-byte tag]`.
    std::vector<std::byte> seal(std::span<const std::byte> plaintext);

    /// Decrypt one record body (ciphertext+tag, without the length prefix) into
    /// `out`. Returns false on authentication failure.
    bool open(std::span<const std::byte> ciphertext, std::vector<std::byte>& out);

    key32         txKey{};
    key32         rxKey{};
    std::uint64_t txCtr = 0;
    std::uint64_t rxCtr = 0;
};

struct handshake_result {
    secure_session session;
    tag32          myConfirmTag{};    // send to the peer
    tag32          expectedPeerTag{}; // compare against the peer's confirm tag
};

// --- node identity (Ed25519, self-certifying) -------------------------------

using id_pubkey = std::array<std::byte, 32>; // Ed25519 public key
using id_seckey = std::array<std::byte, 64>; // Ed25519 secret key (libsodium form)
using seed32    = std::array<std::byte, 32>; // identity seed (persisted form)
using id_sig    = std::array<std::byte, 64>; // Ed25519 detached signature

struct identity_keypair {
    id_pubkey pk{};
    id_seckey sk{};
};

/// Generate a random Ed25519 identity keypair.
identity_keypair gen_identity();

/// Deterministically derive an identity keypair from a 32-byte seed (the
/// persisted form).
identity_keypair identity_from_seed(const seed32& seed);

/// The first half of an Ed25519 secret key is its seed; extract it for storage.
seed32 seed_of(const identity_keypair&);

/// Derive a 16-byte node id from an identity public key (BLAKE2b, truncated).
/// Returned as raw bytes so peer_id stays crypto-agnostic.
std::array<std::byte, 16> id_from_identity(const id_pubkey&);

/// Sign / verify a message with an Ed25519 identity key.
id_sig sign(std::span<const std::byte> msg, const id_seckey& sk);
bool   verify_sig(std::span<const std::byte> msg, const id_sig& sig, const id_pubkey& pk);

/// Perform the X25519 + PSK key agreement. Both peers, given each other's
/// ephemeral public key and the shared group key, derive identical directional
/// session keys and matching confirmation tags. Returns false on an invalid
/// peer key (e.g. low order) or when crypto is unavailable.
bool do_handshake(const ephemeral_keypair& mine, const key32& peerEphPk,
                  const key32& groupKey, const std::string& groupName,
                  handshake_result& out);

/// Constant-time comparison of two equal-length byte ranges.
bool verify_tag(std::span<const std::byte> a, std::span<const std::byte> b);

} // namespace mm::crypto
