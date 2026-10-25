// libsodium backend for wt::security (used where OpenSSL 3.2+ is unavailable). Produces the same
// formats as the OpenSSL backend: Argon2id v1.3 and IETF ChaCha20-Poly1305 (ciphertext || tag).

#include "wt/security/Crypto.h"

#include <sodium.h>

#include <mutex>
#include <stdexcept>

namespace wt::security {
namespace {

void ensureSodium() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (sodium_init() < 0) {
            throw std::runtime_error("libsodium could not be initialised");
        }
    });
}

} // namespace

void Secret::wipe() {
    if (!m_value.empty()) {
        sodium_memzero(m_value.data(), m_value.size());
        m_value.clear();
    }
}

std::optional<Secret> argon2id(std::string_view password, std::string_view salt, const Argon2Params& params,
                               std::size_t length, std::string* error, std::string_view secret,
                               std::string_view associatedData) {
    ensureSodium();
    auto fail = [error](std::string message) -> std::optional<Secret> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };
    if (!secret.empty() || !associatedData.empty()) {
        return fail("the libsodium backend does not support Argon2id secret/associated data");
    }
    if (params.parallelism != 1) {
        return fail("the libsodium backend only supports Argon2id with one lane");
    }
    if (salt.size() != crypto_pwhash_argon2id_SALTBYTES) {
        return fail("Argon2id salts must be 16 bytes with the libsodium backend");
    }
    if (length < 16 || params.iterations < crypto_pwhash_argon2id_OPSLIMIT_MIN ||
        static_cast<std::size_t>(params.memoryKiB) * 1024 < crypto_pwhash_argon2id_MEMLIMIT_MIN) {
        return fail("invalid Argon2id parameters");
    }
    std::string out(length, '\0');
    if (crypto_pwhash(reinterpret_cast<unsigned char*>(out.data()), out.size(), password.data(), password.size(),
                      reinterpret_cast<const unsigned char*>(salt.data()), params.iterations,
                      static_cast<std::size_t>(params.memoryKiB) * 1024, crypto_pwhash_ALG_ARGON2ID13) != 0) {
        sodium_memzero(out.data(), out.size());
        return fail("Argon2id derivation failed (out of memory?)");
    }
    return Secret(std::move(out));
}

std::optional<std::string> aeadEncrypt(std::string_view key, std::string_view nonce, std::string_view plaintext,
                                       std::string_view associatedData, std::string* error) {
    ensureSodium();
    if (key.size() != crypto_aead_chacha20poly1305_ietf_KEYBYTES ||
        nonce.size() != crypto_aead_chacha20poly1305_ietf_NPUBBYTES) {
        if (error) {
            *error = "invalid key or nonce size";
        }
        return std::nullopt;
    }
    std::string out(plaintext.size() + crypto_aead_chacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long length = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(out.data()), &length, reinterpret_cast<const unsigned char*>(plaintext.data()),
        plaintext.size(), reinterpret_cast<const unsigned char*>(associatedData.data()), associatedData.size(), nullptr,
        reinterpret_cast<const unsigned char*>(nonce.data()), reinterpret_cast<const unsigned char*>(key.data()));
    out.resize(static_cast<std::size_t>(length));
    return out;
}

std::optional<Secret> aeadDecrypt(std::string_view key, std::string_view nonce, std::string_view ciphertextAndTag,
                                  std::string_view associatedData) {
    ensureSodium();
    if (key.size() != crypto_aead_chacha20poly1305_ietf_KEYBYTES ||
        nonce.size() != crypto_aead_chacha20poly1305_ietf_NPUBBYTES ||
        ciphertextAndTag.size() < crypto_aead_chacha20poly1305_ietf_ABYTES) {
        return std::nullopt;
    }
    std::string out(ciphertextAndTag.size() - crypto_aead_chacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long length = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(out.data()), &length, nullptr,
            reinterpret_cast<const unsigned char*>(ciphertextAndTag.data()), ciphertextAndTag.size(),
            reinterpret_cast<const unsigned char*>(associatedData.data()), associatedData.size(),
            reinterpret_cast<const unsigned char*>(nonce.data()), reinterpret_cast<const unsigned char*>(key.data())) != 0) {
        return std::nullopt;
    }
    out.resize(static_cast<std::size_t>(length));
    return Secret(std::move(out));
}

std::string randomBytes(std::size_t count) {
    ensureSodium();
    std::string out(count, '\0');
    randombytes_buf(out.data(), out.size());
    return out;
}

std::string cryptoLibraryVersion() {
    return std::string("libsodium ") + sodium_version_string();
}

} // namespace wt::security
