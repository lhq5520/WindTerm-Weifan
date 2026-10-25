#pragma once

#include "wt/core/Status.h"
#include "wt/security/Crypto.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wt::security {

// Encrypted credential store (passwords, key passphrases, proxy passwords).
//
// File format (JSON): a header with the KDF parameters and salt, a nonce and the ciphertext.
// The key is derived from the master password with Argon2id; entries are encrypted together with
// ChaCha20-Poly1305 and the header is authenticated as associated data, so tampering with the
// parameters is detected. Every save uses a fresh nonce; changing the master password also
// changes the salt. Only the derived key is kept in memory, never the master password.
class CredentialVault {
public:
    static constexpr int kVersion = 1;

    struct Entry {
        Secret secret;
        std::string label; // e.g. "alice@example.com password"
        std::int64_t updated = 0;
    };

    // A new, empty vault (written on the first save()).
    static std::unique_ptr<CredentialVault> create(std::string path, std::string_view masterPassword,
                                                   const Argon2Params& params = {}, std::string* error = nullptr);
    // Opens and decrypts an existing vault. Fails with "wrong master password or damaged vault"
    // when authentication fails.
    static std::unique_ptr<CredentialVault> open(std::string path, std::string_view masterPassword,
                                                 std::string* error = nullptr);
    static bool exists(const std::string& path);

    const std::string& path() const { return m_path; }
    const Argon2Params& params() const { return m_params; }

    bool contains(const std::string& id) const { return m_entries.count(id) > 0; }
    std::optional<std::string> secret(const std::string& id) const;
    std::string label(const std::string& id) const;
    void set(const std::string& id, std::string_view secret, std::string label = {});
    bool remove(const std::string& id);
    std::vector<std::string> ids() const;

    core::Status save() const;
    // Re-keys the vault with a new master password (and optionally new parameters) and saves it.
    core::Status changeMasterPassword(std::string_view newPassword, std::optional<Argon2Params> params = {});

    // Fresh random id for a new entry.
    static std::string newId();

private:
    CredentialVault() = default;

    std::string m_path;
    Argon2Params m_params;
    std::string m_salt;
    Secret m_key;
    std::map<std::string, Entry> m_entries;
};

} // namespace wt::security
