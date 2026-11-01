# Building

## Requirements

| Dependency | Version | Needed for |
|---|---|---|
| CMake + Ninja | CMake ≥ 3.21 | everything |
| C++20 compiler | Apple Clang 14+ (Xcode 14+), Clang 15+, GCC 11+, MSVC 2022 | everything |
| Qt | ≥ 6.5 (Core, Gui, Widgets; DBus on Linux/BSD; Test for GUI tests) | the application (`WT_BUILD_APP`) |
| Qt LinguistTools | same as Qt (optional) | the Chinese translation (without it the app is English only) |
| libssh2 | ≥ 1.10 (OpenSSL backend) | SSH sessions and SFTP (`WT_ENABLE_SSH`) |
| OpenSSL ≥ 3.2 **or** libsodium ≥ 1.0.18 | | encrypted credential vault (`WT_ENABLE_VAULT`) |
| zlib | any (optional) | zlib-compressed kitty graphics (`o=z`) |
| OpenSSH server/client binaries | any recent | only to run the SSH integration tests (they start an unprivileged `sshd`) |
| `dbus-daemon`, `xauth` | any (optional) | the Secret Service and X11 forwarding tests (skipped without them) |

Optional features are detected automatically: without libssh2 the app builds without SSH/SFTP,
without a crypto backend it builds without the vault. Force them with `-DWT_ENABLE_SSH=ON` /
`-DWT_ENABLE_VAULT=ON` (configuration then fails if the dependency is missing) or disable them
with `OFF`.

## Quick start

```sh
cmake --preset dev            # configure (Debug) into build/dev
cmake --build --preset dev    # build app + tests
ctest --preset dev            # run all tests
./build/dev/app/WindTerm-Weifan            # Linux
open build/dev/app/WindTerm-Weifan.app     # macOS
```

The configure summary shows what was enabled, e.g.
`WindTerm-Weifan 0.1.0: app=ON tests=ON ssh=ON vault=ON(openssl) ...`.

## Presets

| Preset | Purpose |
|---|---|
| `dev` | Debug build of the app and all tests |
| `release` | RelWithDebInfo build (used by the packaging scripts) |
| `core-only` | Qt-free libraries and their tests only (no Qt needed) |
| `sodium` | Debug build with the vault on libsodium (distributions whose OpenSSL is older than 3.2) |
| `asan` | Debug + AddressSanitizer + UndefinedBehaviorSanitizer |
| `macos-arm64` | Native Apple Silicon build (`CMAKE_OSX_ARCHITECTURES=arm64`, macOS 12+) |
| `macos-universal` | universal2 (arm64 + x86_64); needs universal builds of Qt, libssh2 and OpenSSL |
| `windows-msvc` | Windows x64 with MSVC; libssh2/OpenSSL from vcpkg (`vcpkg.json`, `VCPKG_ROOT`) |
| `windows-mingw-cross` | Compile check of the libraries and tests for Windows with mingw-w64 (Linux/macOS host) |

Options: `WT_BUILD_APP`, `WT_BUILD_TESTS`, `WT_WARNINGS_AS_ERRORS`, `WT_ENABLE_SANITIZERS`,
`WT_ENABLE_SSH` (AUTO/ON/OFF), `WT_ENABLE_VAULT` (AUTO/ON/OFF), `WT_CRYPTO_BACKEND`
(AUTO/openssl/sodium).

Personal settings (Qt location, compiler) belong in a `CMakeUserPresets.json` (ignored by git),
for example:

```json
{
  "version": 3,
  "configurePresets": [
    { "name": "mine", "inherits": "dev", "cacheVariables": { "CMAKE_PREFIX_PATH": "/opt/Qt/6.8.2/gcc_64" } }
  ]
}
```

## macOS (Apple Silicon)

```sh
brew install cmake ninja qt libssh2 openssl@3
cmake --preset macos-arm64 \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix openssl@3);$(brew --prefix libssh2)"
cmake --build --preset macos-arm64
ctest --preset macos-arm64
open build/macos-arm64/app/WindTerm-Weifan.app
lipo -info build/macos-arm64/app/WindTerm-Weifan.app/Contents/MacOS/WindTerm-Weifan   # expect arm64
```

Homebrew's packages on Apple Silicon are arm64-only; for `macos-universal` use the official Qt
installer (universal) and universal builds of libssh2/OpenSSL (e.g. vcpkg with the
`arm64-osx`/`x64-osx` triplets merged by `lipo`). Packaging, signing and notarization:
[PACKAGING.md](PACKAGING.md).

Keyboard notes on macOS: the physical Control key sends terminal control codes (Qt reports it as
Meta), Command is reserved for application shortcuts (⌘T, ⌘D, ⌘F, ...), and Option composes
characters unless *Use Option as Meta key* is enabled in the settings.

## Linux

Debian/Ubuntu:

```sh
sudo apt install cmake ninja-build g++ libssh2-1-dev libsodium-dev zlib1g-dev \
    openssh-server openssh-client dbus-daemon xauth
```

Fedora: `sudo dnf install cmake ninja-build gcc-c++ qt6-qtbase-devel qt6-qttools-devel libssh2-devel
openssl-devel zlib-devel openssh-server dbus-daemon`; Arch: `sudo pacman -S cmake ninja qt6-base
qt6-tools libssh2 openssl zlib openssh`. Package recipes for both and for Flatpak are in
`packaging/` ([PACKAGING.md](PACKAGING.md)).

Qt: `qt6-base-dev` provides Qt 6.6+ on Ubuntu 24.10 / Debian 13 and later. Ubuntu 24.04 ships Qt
6.4, which is below the minimum: use the Qt online installer, `aqtinstall` or Homebrew on Linux and
pass its prefix through `CMAKE_PREFIX_PATH`. Ubuntu 24.04's OpenSSL is 3.0 (no Argon2id), so the
vault uses libsodium there automatically. Without Qt you can still build and test the libraries
with `--preset core-only`.

Verified so far: Ubuntu 24.04 x86_64 with GCC 13.3, Qt 6.12, libssh2 1.11.1, OpenSSL 4.0 and
libsodium 1.0.22 (Homebrew on Linux); OpenSSH 9.6 for the integration tests.

## Windows

```bat
:: "x64 Native Tools Command Prompt for VS 2022"
set VCPKG_ROOT=C:\vcpkg
set CMAKE_PREFIX_PATH=C:\Qt\6.8.2\msvc2022_64
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

vcpkg installs libssh2 and OpenSSL from `vcpkg.json` during configuration. The local terminal uses
ConPTY (Windows 10 1809 or later). The Windows code paths are compile-checked from Linux with the
`windows-mingw-cross` preset (`brew install mingw-w64` or `apt install mingw-w64`); running the
Windows tests needs Windows (see the CI workflow).

## Tests

| Test | Kind | Covers |
|---|---|---|
| `wt_core_tests` | unit | UTF-8, character width, Status, base64/hex, JSON |
| `wt_terminal_tests` | unit | VT parser, terminal model, DCS/OSC 8/52/133, SGR, kitty keyboard (all flags, keypad), modifyOtherKeys, grapheme clusters (2027), damage tracking, reflow (incl. selections and DECSC), search, Sixel, kitty graphics, randomized stress tests |
| `wt_session_tests` | unit | themes, key bindings, settings, session profiles/store, jump chains, backoff, atomic files, session logs, command history, workspace state |
| `wt_security_tests` | unit | Argon2id (RFC 9106), ChaCha20-Poly1305 (RFC 8439), vault round trip, tampering, re-keying, cross-backend fixture |
| `wt_net_tests` | integration | TCP connect/timeout/cancel, SOCKS5 (with auth), HTTP CONNECT, socket pairs, Telnet negotiation, SOCKS server |
| `wt_pty_tests` | integration | POSIX: real child processes on a PTY, Flatpak host commands; Windows: ConPTY |
| `wt_serial_tests` | integration | serial line settings, BREAK, exclusive access (on a pseudo-terminal pair) |
| `wt_ssh_tests` | integration | real `sshd`: host keys (unknown/changed/saved), passphrases, agent, certificates, security keys (software authenticator in a test agent), password failure, shell + resize + exit status, SFTP (incl. folder trees), jump host, proxies, connection loss, abort |
| `wt_ssh_forwarding_tests` | integration | real `sshd`: local, remote and dynamic forwards, agent forwarding, X11 forwarding (cookie rewriting) |
| `wt_app_tests` | GUI | session ↔ PTY ↔ widget: rendering, input, mouse, selection, images, window lifetime |
| `wt_workspace_tests` | GUI | tabs, splits, search bar, themes, key bindings, zoom, links, OSC 52, settings persistence, dialogs |
| `wt_protocol_app_tests` | GUI | Telnet and serial sessions end to end (local Telnet server, PTY pair) |
| `wt_feature_tests` | GUI | session logs, triggers, completion, broadcasting, kitty key events, Chinese translation |
| `wt_credential_tests` | GUI | system credential store (fake Secret Service on a private D-Bus), vault idle lock, workspace restore |
| `wt_ssh_app_tests` | GUI | SSH profiles end to end, host key rejection, vault-stored passphrase, automatic reconnect, jump host, SFTP panel |
| `wt_qt_free_libs` | lint | `libs/**` contains no Qt dependency |

Run a subset: `./build/dev/tests/wt_terminal_tests sgr` (substring filter),
`ctest --preset dev -L unit`, `ctest --preset dev -L ssh`, `ctest --preset dev -LE gui`.
GUI tests run headless with `QT_QPA_PLATFORM=offscreen` (CTest sets this).
SSH tests skip themselves when no `sshd` binary is found.

Set `WT_TEST_SCREENSHOT_DIR=<dir>` when running `wt_app_tests` to save a rendering of the terminal
widget for visual inspection.

## Helper tools

* `tools/gen_char_width.py` regenerates the Unicode width tables.
* `tools/make_icons.py` regenerates the PNG/ICO/ICNS icons from `images/WindTerm_icon_1024x1024.png`
  (standard library only).
* `tools/package/` contains the packaging scripts ([PACKAGING.md](PACKAGING.md)).
* `wt_terminal_bench [MB]` (built with the tests) prints parser + model throughput; use a release build.
