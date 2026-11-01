# Roadmap

Status as of 2026-11-01. "Verified" means covered by automated tests on Linux x86_64 unless noted.

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

- [ ] **macOS on Apple Silicon:** the build before Phase 7 was confirmed to run on an M-series
      Mac. Phase 7 adds macOS-specific code (Keychain store, `IOSSIOSPEED` serial speeds) that has
      not been compiled there yet: build, run the test suite and try the new features; check IME,
      Option/Command handling, Retina rendering.
- [ ] **macOS signing and notarization:** needs a Developer ID certificate and a notarytool
      profile; run `tools/package/package-macos.sh` with `WT_CODESIGN_IDENTITY` and
      `WT_NOTARY_PROFILE`, then test the DMG on a second Mac (Gatekeeper).
- [ ] **Windows:** build with MSVC, run the tests (ConPTY tests exist but have only been
      compile-checked with mingw-w64), test the app, sign with signtool.
- [ ] **First CI run on GitHub** (the workflow has never executed; actions versions and runner
      images may need adjustments).
- [ ] **Distribution packages:** `packaging/` has an RPM spec, a PKGBUILD and a Flatpak manifest,
      and the AppStream metainfo validates, but none of them has been built with `rpmbuild`,
      `makepkg` or `flatpak-builder` yet (not available on the build machine).
- [ ] **Platform code written without the platform:** the macOS Keychain store, the Windows
      Credential Manager store, Windows serial ports and the Windows OpenSSH agent pipe (agent
      forwarding, security keys) are compile-checked at most (mingw-w64 for Windows; the macOS
      parts not at all) and need a run on the real system.

## Phase 7: remaining enhancements (done)

- [x] **Telnet** sessions: RFC 854 with RFC 1143 option negotiation, NAWS (window size), TTYPE,
      TSPEED, NEW-ENVIRON, binary mode, local echo automatic/on/off, CR LF or CR NUL newlines,
      SOCKS5/HTTP proxies
- [x] **Serial** sessions: standard and custom baud rates, data bits, parity, stop bits, RTS/CTS and
      XON/XOFF flow control, BREAK, exclusive access, reopening a device that was unplugged
      (POSIX verified with pseudo-terminals; the Windows backend is compile-checked)
- [x] **SSH forwarding:** local, remote and dynamic (SOCKS4/4a/5) port forwards saved in the
      profile and editable while connected (*Session ▸ Port Forwarding*); agent forwarding; X11
      forwarding with a fake MIT-MAGIC-COOKIE-1 that is replaced by the real one for the local
      display
- [x] **SSH authentication:** OpenSSH user certificates (`<key>-cert.pub` is picked up
      automatically); FIDO/U2F security keys (`sk-ssh-ed25519`, `sk-ecdsa-sha2-nistp256`) held by
      ssh-agent — libssh2 drops their flags and counter, so these signatures are requested from the
      agent directly
- [x] **Session logging** (plain text with optional timestamps, or raw; file name patterns;
      per session or automatic), **triggers** (highlight, notify, beep, send/auto-answer),
      **command completion** from the shell history, **input broadcasting** to the panes of a tab or
      to all tabs
- [x] **Credentials:** the vault locks itself after inactivity (default 30 minutes); passwords can
      go to the system store instead (macOS Keychain, Windows Credential Manager, Secret Service)
- [x] **Session restore:** window layout, tabs, splits and their sessions come back on start
- [x] **Translations:** Simplified Chinese (all 412 strings), language setting and `WT_LANGUAGE`
- [x] **Kitty keyboard flags 2/4/16** (event types, alternate keys, associated text) and keypad keys
      with their KP_* codes
- [x] **Grapheme clusters** as one character (mode 2027): emoji ZWJ sequences, flags, VS16
- [x] **Graphics:** Sixel (VT340 palette, HLS/RGB, raster attributes, DECSDM, XTSMGRAPHICS) and the
      kitty graphics protocol (direct transmission, RGB/RGBA/PNG, chunks, zlib, placements with
      source rectangles, scaling, offsets and z-index, deletion, queries)
- [x] **SFTP:** whole folders upload and download
- [x] **Reflow:** the DECSC saved cursor follows its text
- [x] **Packaging:** RPM spec, PKGBUILD, Flatpak manifest, AppStream metainfo; inside Flatpak local
      shells run on the host through `flatpak-spawn --host`

## Not implemented

- [ ] GSSAPI/Kerberos authentication: libssh2 has no GSSAPI support.
- [ ] Security keys without an agent (talking to the authenticator needs libfido2): add the key to
      ssh-agent (`ssh-add ~/.ssh/id_ed25519_sk`).
- [ ] Glyph atlas / GPU rendering (painting uses QPainter with Qt's glyph cache);
      benchmark-driven tuning with `benchmark/urandom_test.sh`.
- [ ] Kitty graphics: animation, Unicode placeholders, and file / shared-memory transmission (the
      last on purpose: a remote program could otherwise display local files).
- [ ] Translations other than Simplified Chinese.

## Known limitations

- When a reflow makes the text below the cursor taller than the screen, the lines that no longer
  fit below the screen are dropped (the cursor line stays visible), as in most terminals.
- Kitty keyboard protocol: shifted symbols are reported with the shifted code point (the keyboard
  layout's base key is not known); keypad cursor keys (NumLock off) are reported as ordinary
  cursor keys, because macOS marks the main arrow keys as keypad keys too.
- Block selections and selections on the alternate screen are dropped on resize.
- Images (Sixel and kitty) are removed when the number of columns changes, since the text they
  belong to is rewrapped. Kitty images with a negative z-index are drawn below the text and also
  below non-default cell backgrounds.
- Command completion learns only commands that the shell echoed; input typed at prompts that do
  not echo (passwords) is never recorded.
- The AppImage needs the host's graphics libraries (libEGL/libGL), like any AppImage.
