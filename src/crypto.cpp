#include "crypto.hpp"

#include <cstring>

#ifdef MULTIMASTER_HAVE_CRYPTO
#include <sodium.h>
#endif

namespace mm::crypto {

#ifdef MULTIMASTER_HAVE_CRYPTO

namespace {

const unsigned char* uc(const std::byte* p) {
    return reinterpret_cast<const unsigned char*>(p);
}
unsigned char* uc(std::byte* p) { return reinterpret_cast<unsigned char*>(p); }

// 96-bit AEAD nonce = 4 zero bytes + 64-bit big-endian record counter. Distinct
// per direction (separate keys) and never repeated under one key.
void nonce_from_counter(unsigned char nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES],
                        std::uint64_t ctr) {
    std::memset(nonce, 0, crypto_aead_chacha20poly1305_ietf_NPUBBYTES);
    for (int i = 0; i < 8; ++i)
        nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES - 1 - i] =
            static_cast<unsigned char>((ctr >> (i * 8)) & 0xFF);
}

constexpr char kPskCtx[] = "multimaster-psk-v1"; // BLAKE2b key for PSK -> master

} // namespace

bool init() { return sodium_init() >= 0; }
bool available() { return true; }

key32 derive_group_key(const std::string& passphrase) {
    key32 out{};
    crypto_generichash(uc(out.data()), out.size(),
                       reinterpret_cast<const unsigned char*>(passphrase.data()),
                       passphrase.size(),
                       reinterpret_cast<const unsigned char*>(kPskCtx), sizeof(kPskCtx) - 1);
    return out;
}

key32 derive_discovery_key(const key32& groupKey) {
    key32 out{};
    static const unsigned char label[] = "discovery";
    crypto_generichash(uc(out.data()), out.size(), label, sizeof(label) - 1,
                       uc(groupKey.data()), groupKey.size());
    return out;
}

mac16 discovery_tag(std::span<const std::byte> msg, const key32& discoveryKey) {
    mac16 out{};
    crypto_generichash(uc(out.data()), out.size(), uc(msg.data()), msg.size(),
                       uc(discoveryKey.data()), discoveryKey.size());
    return out;
}

bool discovery_verify(std::span<const std::byte> msg, std::span<const std::byte> tag,
                      const key32& discoveryKey) {
    if (tag.size() != 16) return false;
    mac16 expected = discovery_tag(msg, discoveryKey);
    return sodium_memcmp(expected.data(), tag.data(), 16) == 0;
}

ephemeral_keypair gen_ephemeral() {
    ephemeral_keypair kp;
    crypto_box_keypair(uc(kp.pk.data()), uc(kp.sk.data()));
    return kp;
}

bool verify_tag(std::span<const std::byte> a, std::span<const std::byte> b) {
    if (a.size() != b.size()) return false;
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool do_handshake(const ephemeral_keypair& mine, const key32& peerEphPk,
                  const key32& groupKey, const std::string& groupName,
                  handshake_result& out) {
    // X25519 shared secret; rejects low-order / invalid peer keys.
    std::array<std::byte, 32> dh{};
    if (crypto_scalarmult(uc(dh.data()), uc(mine.sk.data()), uc(peerEphPk.data())) != 0)
        return false;

    // Order the two ephemeral pubkeys so both ends derive the same transcript.
    const bool  iAmLo = std::memcmp(mine.pk.data(), peerEphPk.data(), 32) < 0;
    const key32& lo   = iAmLo ? mine.pk : peerEphPk;
    const key32& hi   = iAmLo ? peerEphPk : mine.pk;

    // master = BLAKE2b(key = groupKey, dh || lo || hi || groupName)
    std::array<std::byte, 64> master{};
    crypto_generichash_state st;
    crypto_generichash_init(&st, uc(groupKey.data()), groupKey.size(), master.size());
    crypto_generichash_update(&st, uc(dh.data()), dh.size());
    crypto_generichash_update(&st, uc(lo.data()), lo.size());
    crypto_generichash_update(&st, uc(hi.data()), hi.size());
    crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(groupName.data()),
                              groupName.size());
    crypto_generichash_final(&st, uc(master.data()), master.size());

    // Directional session keys: kA||kB = BLAKE2b(key = master, "session"). The
    // lower-pubkey side transmits with kA / receives with kB; the peer mirrors.
    std::array<std::byte, 64> skbytes{};
    static const unsigned char skLabel[] = "mm-session-keys-v1";
    crypto_generichash(uc(skbytes.data()), skbytes.size(), skLabel, sizeof(skLabel) - 1,
                       uc(master.data()), master.size());
    key32 kA{}, kB{};
    std::memcpy(kA.data(), skbytes.data(), 32);
    std::memcpy(kB.data(), skbytes.data() + 32, 32);
    out.session.txKey = iAmLo ? kA : kB;
    out.session.rxKey = iAmLo ? kB : kA;

    // Confirmation tags: tag(pk) = BLAKE2b(key = BLAKE2b(master,"confirm"), pk).
    key32                      confirmBase{};
    static const unsigned char cLabel[] = "mm-confirm-v1";
    crypto_generichash(uc(confirmBase.data()), confirmBase.size(), cLabel, sizeof(cLabel) - 1,
                       uc(master.data()), master.size());
    crypto_generichash(uc(out.myConfirmTag.data()), out.myConfirmTag.size(), uc(mine.pk.data()),
                       mine.pk.size(), uc(confirmBase.data()), confirmBase.size());
    crypto_generichash(uc(out.expectedPeerTag.data()), out.expectedPeerTag.size(),
                       uc(peerEphPk.data()), peerEphPk.size(), uc(confirmBase.data()),
                       confirmBase.size());

    sodium_memzero(dh.data(), dh.size());
    sodium_memzero(master.data(), master.size());
    sodium_memzero(skbytes.data(), skbytes.size());
    return true;
}

std::vector<std::byte> secure_session::seal(std::span<const std::byte> plaintext) {
    unsigned char nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
    nonce_from_counter(nonce, txCtr++);

    const std::size_t     ctLen = plaintext.size() + crypto_aead_chacha20poly1305_ietf_ABYTES;
    std::vector<std::byte> rec(4 + ctLen);
    rec[0] = static_cast<std::byte>((ctLen >> 24) & 0xFF);
    rec[1] = static_cast<std::byte>((ctLen >> 16) & 0xFF);
    rec[2] = static_cast<std::byte>((ctLen >> 8) & 0xFF);
    rec[3] = static_cast<std::byte>(ctLen & 0xFF);

    unsigned long long outLen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(uc(rec.data() + 4), &outLen, uc(plaintext.data()),
                                              plaintext.size(), nullptr, 0, nullptr, nonce,
                                              uc(txKey.data()));
    rec.resize(4 + static_cast<std::size_t>(outLen));
    return rec;
}

bool secure_session::open(std::span<const std::byte> ciphertext, std::vector<std::byte>& out) {
    if (ciphertext.size() < crypto_aead_chacha20poly1305_ietf_ABYTES) return false;
    unsigned char nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
    nonce_from_counter(nonce, rxCtr);

    out.resize(ciphertext.size() - crypto_aead_chacha20poly1305_ietf_ABYTES);
    unsigned long long outLen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(uc(out.data()), &outLen, nullptr,
                                                  uc(ciphertext.data()), ciphertext.size(),
                                                  nullptr, 0, nonce, uc(rxKey.data())) != 0)
        return false;
    out.resize(static_cast<std::size_t>(outLen));
    ++rxCtr;
    return true;
}

identity_keypair gen_identity() {
    identity_keypair kp;
    crypto_sign_keypair(uc(kp.pk.data()), uc(kp.sk.data()));
    return kp;
}

identity_keypair identity_from_seed(const seed32& seed) {
    identity_keypair kp;
    crypto_sign_seed_keypair(uc(kp.pk.data()), uc(kp.sk.data()), uc(seed.data()));
    return kp;
}

seed32 seed_of(const identity_keypair& kp) {
    seed32 s{};
    crypto_sign_ed25519_sk_to_seed(uc(s.data()), uc(kp.sk.data()));
    return s;
}

std::array<std::byte, 16> id_from_identity(const id_pubkey& pk) {
    std::array<std::byte, 16> out{};
    crypto_generichash(uc(out.data()), out.size(), uc(pk.data()), pk.size(), nullptr, 0);
    return out;
}

id_sig sign(std::span<const std::byte> msg, const id_seckey& sk) {
    id_sig sig{};
    crypto_sign_detached(uc(sig.data()), nullptr, uc(msg.data()), msg.size(), uc(sk.data()));
    return sig;
}

bool verify_sig(std::span<const std::byte> msg, const id_sig& sig, const id_pubkey& pk) {
    return crypto_sign_verify_detached(uc(sig.data()), uc(msg.data()), msg.size(),
                                       uc(pk.data())) == 0;
}

#else // !MULTIMASTER_HAVE_CRYPTO — safe stubs (a secured mesh refuses to start)

bool  init() { return false; }
bool  available() { return false; }
key32 derive_group_key(const std::string&) { return {}; }
key32 derive_discovery_key(const key32&) { return {}; }
mac16 discovery_tag(std::span<const std::byte>, const key32&) { return {}; }
bool  discovery_verify(std::span<const std::byte>, std::span<const std::byte>, const key32&) {
    return false;
}
ephemeral_keypair gen_ephemeral() { return {}; }
bool              verify_tag(std::span<const std::byte>, std::span<const std::byte>) { return false; }
bool              do_handshake(const ephemeral_keypair&, const key32&, const key32&,
                               const std::string&, handshake_result&) {
    return false;
}
std::vector<std::byte> secure_session::seal(std::span<const std::byte>) { return {}; }
bool secure_session::open(std::span<const std::byte>, std::vector<std::byte>&) { return false; }
identity_keypair gen_identity() { return {}; }
identity_keypair identity_from_seed(const seed32&) { return {}; }
seed32           seed_of(const identity_keypair&) { return {}; }
std::array<std::byte, 16> id_from_identity(const id_pubkey&) { return {}; }
id_sig sign(std::span<const std::byte>, const id_seckey&) { return {}; }
bool   verify_sig(std::span<const std::byte>, const id_sig&, const id_pubkey&) { return false; }

#endif

} // namespace mm::crypto
