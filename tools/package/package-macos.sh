#!/usr/bin/env bash
# Builds, signs, notarizes and packages WindTerm-Weifan for macOS (Apple Silicon by default).
#
#   tools/package/package-macos.sh [--universal] [--skip-tests]
#
# Steps: configure + build (preset macos-arm64, or macos-universal), run the tests, bundle Qt with
# macdeployqt, sign every nested binary and the app with the hardened runtime, build a DMG, then
# notarize and staple it.
#
# Environment:
#   WT_CODESIGN_IDENTITY  "Developer ID Application: Name (TEAMID)"; unset = ad-hoc signature
#                         (runs locally, but Gatekeeper rejects it on other Macs)
#   WT_NOTARY_PROFILE     keychain profile created with `xcrun notarytool store-credentials`;
#                         unset = skip notarization
#   QT_DIR / CMAKE_PREFIX_PATH  Qt 6 location (default: $(brew --prefix qt))
#   WT_DIST_DIR           output directory (default <repo>/dist)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
preset="macos-arm64"
tests=1
for arg in "$@"; do
    case "$arg" in
        --universal) preset="macos-universal" ;;
        --skip-tests) tests=0 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done
[[ "$(uname -s)" == Darwin ]] || { echo "run this on macOS" >&2; exit 1; }

dist="${WT_DIST_DIR:-$root/dist}"
qt="${QT_DIR:-$(brew --prefix qt 2>/dev/null || true)}"
prefix_path="${CMAKE_PREFIX_PATH:-$qt;$(brew --prefix openssl@3 2>/dev/null || true);$(brew --prefix libssh2 2>/dev/null || true)}"
identity="${WT_CODESIGN_IDENTITY:--}"
entitlements="$root/app/resources/macos/entitlements.plist"

cd "$root"
mkdir -p "$dist"
cmake --preset "$preset" -DWT_WARNINGS_AS_ERRORS=ON -DCMAKE_PREFIX_PATH="$prefix_path"
cmake --build --preset "$preset"
if [[ $tests == 1 ]]; then
    ctest --preset "$preset"
fi

build="$root/build/$preset"
app="$build/app/WindTerm-Weifan.app"
staging="$dist/macos"
rm -rf "$staging"
mkdir -p "$staging"
cp -R "$app" "$staging/"
app="$staging/WindTerm-Weifan.app"

macdeployqt="${MACDEPLOYQT:-$qt/bin/macdeployqt}"
"$macdeployqt" "$app" -always-overwrite -verbose=1

echo "Architectures:"
lipo -info "$app/Contents/MacOS/WindTerm-Weifan"
if [[ $preset == macos-arm64 ]]; then
    lipo -info "$app/Contents/MacOS/WindTerm-Weifan" | grep -q arm64
fi

# Sign inside-out: dylibs, plugins and frameworks first, then the app bundle.
sign() {
    codesign --force --timestamp=none --options runtime --sign "$identity" "$@"
}
if [[ $identity != - ]]; then
    sign() { codesign --force --timestamp --options runtime --sign "$identity" "$@"; }
fi
while IFS= read -r -d '' binary; do
    sign "$binary"
done < <(find "$app/Contents" \( -name '*.dylib' -o -name '*.so' \) -type f -print0)
while IFS= read -r -d '' framework; do
    sign "$framework"
done < <(find "$app/Contents/Frameworks" -maxdepth 1 -name '*.framework' -print0 2>/dev/null)
sign --entitlements "$entitlements" "$app"
codesign --verify --deep --strict --verbose=2 "$app"

version="$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -1)"
arch_suffix="$([[ $preset == macos-universal ]] && echo universal || echo arm64)"
dmg="$dist/WindTerm-Weifan-${version:-dev}-macOS-$arch_suffix.dmg"
rm -f "$dmg"
ln -s /Applications "$staging/Applications"
hdiutil create -volname "WindTerm-Weifan" -srcfolder "$staging" -ov -format UDZO "$dmg"
rm "$staging/Applications"
if [[ $identity != - ]]; then
    codesign --force --timestamp --sign "$identity" "$dmg"
fi

if [[ -n "${WT_NOTARY_PROFILE:-}" ]]; then
    [[ $identity != - ]] || { echo "notarization needs WT_CODESIGN_IDENTITY" >&2; exit 1; }
    xcrun notarytool submit "$dmg" --keychain-profile "$WT_NOTARY_PROFILE" --wait
    xcrun stapler staple "$dmg"
    xcrun stapler validate "$dmg"
    spctl --assess --type open --context context:primary-signature --verbose=2 "$dmg"
else
    echo "WT_NOTARY_PROFILE not set: skipped notarization"
fi
echo "Created $dmg"
