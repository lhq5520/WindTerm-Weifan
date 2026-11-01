#pragma once

#include "wt/core/Status.h"

#include <optional>
#include <string>

namespace wt::platform {

// The operating system's credential store for generic passwords: the macOS Keychain (login
// keychain, kSecClassGenericPassword) or the Windows Credential Manager (CRED_TYPE_GENERIC,
// "<service>/<account>" targets). On other systems available() is false; the application uses the
// freedesktop Secret Service there (over D-Bus).
namespace keychain {

bool available();
// Human-readable name of the store ("macOS Keychain", "Windows Credential Manager").
std::string storeName();

core::Status store(const std::string& service, const std::string& account, const std::string& label,
                   const std::string& secret);
// nullopt with an ok `status` means "no such item".
std::optional<std::string> load(const std::string& service, const std::string& account, core::Status* status = nullptr);
core::Status remove(const std::string& service, const std::string& account);

} // namespace keychain
} // namespace wt::platform
