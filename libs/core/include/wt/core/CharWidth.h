#pragma once

namespace wt::core {

// Number of terminal columns occupied by `cp`:
//   0 for combining marks / zero-width characters,
//   2 for East Asian Wide/Fullwidth characters and wide emoji,
//   1 otherwise.
// Control characters return 0; callers are expected to handle them separately.
int charWidth(char32_t cp);

} // namespace wt::core
