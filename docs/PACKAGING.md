# Packaging and release

The scripts in `tools/package/` build a release, run the tests and produce distributable
packages in `dist/`. CI (`.github/workflows/ci.yml`) runs them on every push and attaches the
results to a draft GitHub release for `v*` tags.

## Linux

```sh
tools/package/package-linux.sh             # dist/*.tar.gz and *.deb (CPack)
tools/package/package-linux.sh --appimage  # additionally an AppImage via linuxdeploy + Qt plugin
```

* The tarball and the .deb install `bin/WindTerm-Weifan`, a desktop entry and hicolor icons. The
  .deb depends on the distribution's Qt 6 (≥ 6.5), libssh2 and libsodium/libssl; adjust
  `CPACK_DEBIAN_PACKAGE_DEPENDS` in `cmake/WtPackaging.cmake` for other targets.
* The AppImage bundles Qt, libssh2 and the crypto library. Like every AppImage it relies on the
  host for the graphics stack (`libEGL`, `libGL`, X11/Wayland client libraries), which desktop
  systems provide. It runs without FUSE when `APPIMAGE_EXTRACT_AND_RUN=1` is set.
* Verified on Ubuntu 24.04 x86_64: tarball, .deb and AppImage were produced; the AppImage starts,
  runs a shell and exits cleanly (headless, with the host's EGL library available).

## macOS (Apple Silicon)

```sh
brew install cmake ninja qt libssh2 openssl@3
tools/package/package-macos.sh              # arm64
tools/package/package-macos.sh --universal  # universal2 (needs universal dependencies)
```

Steps performed: build + tests (`macos-arm64` preset), `macdeployqt` (bundles Qt frameworks and
plugins, plus libssh2/libcrypto), architecture check with `lipo`, signing of every nested dylib,
plugin and framework and then the bundle with the hardened runtime
(`app/resources/macos/entitlements.plist`), `codesign --verify --deep --strict`, a compressed DMG
with an `/Applications` link, and — when configured — notarization and stapling.

Signing and notarization are controlled by environment variables:

| Variable | Meaning |
|---|---|
| `WT_CODESIGN_IDENTITY` | e.g. `Developer ID Application: Jane Doe (TEAMID)`. Unset: ad-hoc signature (works on the build machine only). |
| `WT_NOTARY_PROFILE` | keychain profile for `xcrun notarytool`, created once with `xcrun notarytool store-credentials <name> --apple-id ... --team-id ... --password <app-specific password>`. Unset: no notarization. |
| `QT_DIR`, `CMAKE_PREFIX_PATH` | Qt (and other dependencies) location; defaults to Homebrew. |

After notarization the script runs `stapler validate` and `spctl --assess`. In CI, provide the
certificate through the runner keychain and set the `MACOS_CODESIGN_IDENTITY` /
`MACOS_NOTARY_PROFILE` secrets.

**Not yet verified:** this script has not been run on real Apple hardware in this repository's
history (it was written and linted on Linux). The first run on a Mac — and on the `macos-14` CI
runner — is the outstanding check, together with a manual test of the app on Apple Silicon.

## Windows

From a "x64 Native Tools Command Prompt for VS 2022" (or after `ilammy/msvc-dev-cmd` in CI):

```powershell
$env:VCPKG_ROOT = 'C:\vcpkg'
$env:CMAKE_PREFIX_PATH = 'C:\Qt\6.8.2\msvc2022_64'
pwsh tools/package/package-windows.ps1
```

The script configures the `windows-msvc` preset (vcpkg installs libssh2 and OpenSSL), builds,
runs the tests (including ConPTY), installs to `dist/WindTerm-Weifan`, runs `windeployqt`, copies
the vcpkg DLLs, optionally signs every `.exe`/`.dll` with `signtool`
(`WT_SIGN_CERT_THUMBPRINT`, `WT_SIGN_TIMESTAMP_URL`) and creates a ZIP (plus an NSIS installer if
`makensis` is installed).

**Not yet verified:** no Windows machine was available while writing it; the Windows sources are
compile-checked with mingw-w64 (`windows-mingw-cross` preset) and the full build/test/package path
runs in the `windows` CI job.

## Version numbers

The version comes from `project(... VERSION x.y.z)` in the top-level `CMakeLists.txt`; it is
used for the bundle/executable metadata, CPack file names and `--version`.
