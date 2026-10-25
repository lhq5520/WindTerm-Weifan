#pragma once

#include "wt/core/Status.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace wt::platform {

// Paths are UTF-8 strings at the API boundary; this converts them for std::filesystem (which uses
// UTF-16 on Windows).
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path& path);

// Reads a whole file; nullopt if it does not exist or cannot be read (`error` explains why).
std::optional<std::string> readFile(std::string_view path, std::string* error = nullptr);

// Writes `data` to a temporary file next to `path`, flushes it to disk and renames it over `path`,
// so readers never see a partially written file. With `privateFile` the file is created with
// owner-only permissions (0600 on POSIX) because it may contain secrets. Parent directories are
// created as needed.
core::Status writeFileAtomic(std::string_view path, std::string_view data, bool privateFile = false);

// Appends `data` to `path` (created with owner-only permissions when `privateIfNew`). Follows
// symlinks and never truncates, so concurrent appenders (e.g. OpenSSH) do not lose lines.
core::Status appendToFile(std::string_view path, std::string_view data, bool privateIfNew = false);

// The user's home directory ($HOME / %USERPROFILE%), empty if unknown.
std::string homeDirectory();

} // namespace wt::platform
