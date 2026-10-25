#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wt::security {

// A byte string that is wiped (OPENSSL_cleanse) when destroyed or reassigned.
class Secret {
public:
    Secret() = default;
    explicit Secret(std::string value) : m_value(std::move(value)) {}
    ~Secret() { wipe(); }
    Secret(const Secret& other) : m_value(other.m_value) {}
    // Moves copy and wipe: a moved-from short string keeps its bytes in the SSO buffer.
    Secret(Secret&& other) noexcept : m_value(other.m_value) { other.wipe(); }
    Secret& operator=(const Secret& other) {
        if (this != &other) {
            wipe();
            m_value = other.m_value;
        }
        return *this;
    }
    Secret& operator=(Secret&& other) noexcept {
        if (this != &other) {
            wipe();
            m_value = other.m_value;
            other.wipe();
        }
        return *this;
    }

    const std::string& value() const { return m_value; }
    std::string_view view() const { return m_value; }
    std::size_t size() const { return m_value.size(); }
    bool empty() const { return m_value.empty(); }
    void wipe();

private:
    std::string m_value;
};

// Argon2id cost parameters (RFC 9106). The defaults follow the RFC's second recommended option
// (64 MiB, 3 passes) with a single lane.
struct Argon2Params {
    std::uint32_t memoryKiB = 64 * 1024;
    std::uint32_t iterations = 3;
    std::uint32_t parallelism = 1;
    friend bool operator==(const Argon2Params&, const Argon2Params&) = default;
};

// Derives `length` bytes with Argon2id. `secret` and `associatedData` are the optional K and X
// inputs of RFC 9106. Requires OpenSSL 3.2 or later.
std::optional<Secret> argon2id(std::string_view password, std::string_view salt, const Argon2Params& params,
                               std::size_t length, std::string* error = nullptr, std::string_view secret = {},
                               std::string_view associatedData = {});

inline constexpr std::size_t kAeadKeySize = 32;
inline constexpr std::size_t kAeadNonceSize = 12;
inline constexpr std::size_t kAeadTagSize = 16;

// ChaCha20-Poly1305 (RFC 8439). Returns ciphertext || tag.
std::optional<std::string> aeadEncrypt(std::string_view key, std::string_view nonce, std::string_view plaintext,
                                       std::string_view associatedData, std::string* error = nullptr);
// Verifies the tag and decrypts; nullopt if the key, nonce, data or AAD do not match.
std::optional<Secret> aeadDecrypt(std::string_view key, std::string_view nonce, std::string_view ciphertextAndTag,
                                  std::string_view associatedData);

// Cryptographically secure random bytes.
std::string randomBytes(std::size_t count);

// Linked crypto library, e.g. "OpenSSL 3.6.0 1 Oct 2025".
std::string cryptoLibraryVersion();

} // namespace wt::security
