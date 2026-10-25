# Roadmap

Status as of 2026-10-25. "Verified" means covered by automated tests on Linux x86_64 unless noted.

## Phase 0: foundation (done)

- [x] CMake project and presets, layered layout (`libs/*` Qt-free, `app/` Qt 6)
- [x] Strict warnings (`-Werror` in CI), ASan/UBSan preset, Qt-free architecture check
- [x] Dependency-free test harness + CTest
- [x] Legacy `src/` preserved and documented

## Phase 1: local terminal MVP (done)

- [x] POSIX PTY backend, VT500 parser, xterm-256color model, key encoding, Qt rendering/IME/CLI

## Phase 2: daily-driver terminal (done)

- [x] PTY lifecycle hardening (hang-up handling, SIGHUP→SIGKILL, bounded drain, pixel sizes)
- [x] Window size propagation (TIOCSWINSZ / ConPTY resize / SSH window-change)
- [x] VT100/xterm state machine incl. DCS: DECRQSS, XTGETTCAP; OSC 4/7/8/10/11/12/52/104/110-112
- [x] Screen buffer with absolute rows, scrollback limit, alternate screen, DECDWL/DECDHL
- [x] Unicode: generated width tables, grapheme clusters, wide characters, wide-char padding
- [x] SGR: 256 colours / truecolor, underline styles (4:x, 21) and colour (58/59), overline (53/55)
- [x] Keyboard: xterm modifiers, keypad, kitty keyboard protocol (disambiguate, report-all-keys),
      modifyOtherKeys 1/2; mouse: X10/1000/1002/1003 × default/UTF-8/SGR/urxvt
- [x] Selection (character/word/line/block), copy/paste, primary selection, bracketed paste,
      copy-on-select; selections survive reflow
- [x] Reflow of wrapped lines on resize (cursor and selection mapped through)
- [x] Rendering: damage tracking (only changed rows repainted), synchronized output (?2026),
      hyperlinks (OSC 8 + URL detection), search highlights, cursor blink, visual bell
- [x] Stability: randomized parser/reflow/selection stress test; ASan/UBSan clean; release build
      (-O2) warning-free with `-Wnull-dereference`; independent code review findings fixed with
      regression tests
- [x] Throughput: ASCII fast path and scroll-line recycling (`wt_terminal_bench`: ~33 MB/s
      ASCII, ~57 MB/s SGR-heavy, ~29 MB/s CJK in the model, release build, this machine)

## Phase 3: workspace (done)

- [x] Tabs (movable, closable, activity marker) and split panes (nested, focus cycling, collapse)
- [x] Scrollback search: literal/regex (PCRE2 via Qt)/case/whole word, highlight all, live refresh
- [x] Themes: 11 built-in + custom themes in settings.json, per-session override
- [x] Configurable shortcuts (keymap with conflict detection; macOS and standard defaults)
- [x] Settings dialog + settings.json (validated, clamped, atomic writes)
- [x] Session profiles: local and SSH, folders, session manager, `--profile`

## Phase 4: SSH (done)

- [x] libssh2-based client layer (`libs/ssh`), legacy libssh fork not used
- [x] known_hosts verification (unknown → confirm/save, changed → warn/replace, hashed entries)
- [x] Authentication: agent, key files (encrypted keys with passphrase prompts), password,
      keyboard-interactive, automatic order
- [x] Interactive shell with PTY, environment, remote command, resize, exit status, keep-alives
- [x] SFTP: list/stat/realpath/mkdir/rmdir/remove/rename, upload/download with progress and
      cancel; GUI panel with drag & drop and recursive delete

## Phase 5: security and resilience (done)

- [x] Credential vault: Argon2id + ChaCha20-Poly1305, OpenSSL ≥ 3.2 or libsodium (same format),
      tamper detection, re-keying, vault-backed passwords/passphrases in session profiles
- [x] Session persistence (sessions.json, secrets only by reference)
- [x] Automatic reconnect with exponential backoff and jitter; manual reconnect
- [x] Proxies: SOCKS5 (incl. user/password), HTTP CONNECT (Basic auth), jump host chains

## Phase 6: platforms and distribution (done, with external verification pending)

- [x] Platform boundaries: POSIX PTY (Linux/macOS), ConPTY (Windows 10 1809+), sockets, files
- [x] Presets for Linux, macOS arm64/universal, Windows MSVC+vcpkg, mingw cross-check, sodium
- [x] CI workflow: Linux, ASan/UBSan, mingw cross-compile, macOS arm64, Windows MSVC, packages,
      draft releases on tags (validated with actionlint)
- [x] Packaging: CPack (tar.gz/.deb/DMG/ZIP/NSIS), AppImage, macdeployqt + signing + notarization
      script, windeployqt + signtool script, icons generated for all platforms

### Remaining external verification (cannot be done on the Linux build machine)

- [ ] **macOS on Apple Silicon:** build, run the test suite and use the app on a real M-series Mac
      (or the `macos-14` CI runner); check IME, Option/Command handling, Retina rendering.
- [ ] **macOS signing and notarization:** needs a Developer ID certificate and a notarytool
      profile; run `tools/package/package-macos.sh` with `WT_CODESIGN_IDENTITY` and
      `WT_NOTARY_PROFILE`, then test the DMG on a second Mac (Gatekeeper).
- [ ] **Windows:** build with MSVC, run the tests (ConPTY tests exist but have only been
      compile-checked with mingw-w64), test the app, sign with signtool.
- [ ] **First CI run on GitHub** (the workflow has never executed; actions versions and runner
      images may need adjustments).
- [ ] Distribution packages on other distributions (Fedora/Arch package metadata, Flatpak).

## Not implemented yet

- [ ] Telnet and serial sessions (port of `src/Protocols/TelnetProtocol`)
- [ ] Session logging, triggers, auto-completion, broadcast input
- [ ] Port forwarding UI (local/remote/dynamic); X11 and agent forwarding
- [ ] SSH certificates, FIDO2/sk keys, GSSAPI
- [ ] Vault auto-lock timeout, OS keychain integration (macOS Keychain, Windows Credential Manager,
      Secret Service)
- [ ] Kitty keyboard flags 2/4/16 (key release events, alternate keys, associated text)
- [ ] Emoji ZWJ sequences rendered as one glyph (widths follow wcwidth, so a ZWJ sequence occupies
      the sum of its parts, matching what shells compute)
- [ ] Glyph atlas / GPU rendering; benchmark-driven tuning (`benchmark/urandom_test.sh`)
- [ ] Sixel / kitty graphics protocol
- [ ] Restoring the window layout and open sessions on restart
- [ ] Translations

## Known limitations

- Saved cursors (DECSC) other than the one used by `?1049` are clamped, not reflowed.
- When a reflow makes the text below the cursor taller than the screen, the lines that no longer
  fit below the screen are dropped (the cursor line stays visible), as in most terminals.
- Kitty keyboard protocol: shifted symbols are reported with the shifted code point (the keyboard
  layout's base key is not known); keypad keys use legacy encodings.
- Block selections and selections on the alternate screen are dropped on resize.
- The SFTP panel transfers files, not whole directories (deleting directories is recursive).
- The AppImage needs the host's graphics libraries (libEGL/libGL), like any AppImage.
