#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wt::core {

// RFC 4648 base64 with padding.
std::string base64Encode(std::string_view bytes);
// Accepts padded or unpadded input and ignores ASCII whitespace; nullopt on any other invalid
// character or impossible length.
std::optional<std::string> base64Decode(std::string_view text);

// Lower-case hexadecimal.
std::string hexEncode(std::string_view bytes);
std::optional<std::string> hexDecode(std::string_view text);

} // namespace wt::core
