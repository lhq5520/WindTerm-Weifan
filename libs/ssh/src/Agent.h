#pragma once

// A small ssh-agent client (draft-miller-ssh-agent) for what libssh2's agent support lacks:
// signatures of FIDO/U2F security keys, whose flags and counter libssh2 1.11 drops.

#include "wt/core/Status.h"
#include "wt/net/Socket.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wt::ssh::detail {

// Connects to the user's agent: $SSH_AUTH_SOCK, or the OpenSSH agent's pipe on Windows.
core::Status connectAgent(net::Socket& out);

// True for security key types (sk-ssh-ed25519@openssh.com, sk-ecdsa-sha2-nistp256@openssh.com and
// their certificates).
bool isSecurityKeyType(std::string_view keyType);
// The key type (the first string) of a public key blob; empty if the blob is malformed.
std::string keyBlobType(std::string_view blob);

// Reads an SSH string at `pos` (advancing it); false if the data is too short.
bool readSshString(std::string_view data, std::size_t& pos, std::string_view& out);

// Asks the agent to sign `data` with `keyBlob`. Returns the agent's signature blob (string
// format, string signature [, byte flags, uint32 counter]). A security key may wait for a touch,
// so this waits up to `timeout`.
std::optional<std::string> agentSign(std::string_view keyBlob, std::string_view data, std::uint32_t flags,
                                     std::chrono::milliseconds timeout, std::string* error);

} // namespace wt::ssh::detail
