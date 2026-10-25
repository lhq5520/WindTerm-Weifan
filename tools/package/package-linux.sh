#!/usr/bin/env bash
# Builds a release of WindTerm-Weifan and packages it for Linux.
#
#   tools/package/package-linux.sh [--appimage] [--skip-tests]
#
# Produces dist/WindTerm-Weifan-<version>-Linux-<arch>.tar.gz and a .deb (CPack). With --appimage
# it also downloads linuxdeploy + its Qt plugin (into dist/.tools) and builds a self-contained
# AppImage that bundles Qt, libssh2 and the crypto library.
#
# Environment: WT_PRESET (default "release"), WT_DIST_DIR (default "<repo>/dist"),
# CMAKE_PREFIX_PATH (where Qt 6 lives, if not found automatically).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
preset="${WT_PRESET:-release}"
dist="${WT_DIST_DIR:-$root/dist}"
appimage=0
tests=1
for arg in "$@"; do
    case "$arg" in
        --appimage) appimage=1 ;;
        --skip-tests) tests=0 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

cd "$root"
mkdir -p "$dist"
cmake --preset "$preset" -DWT_WARNINGS_AS_ERRORS=ON -DCMAKE_INSTALL_PREFIX=/usr
cmake --build --preset "$preset"
if [[ $tests == 1 ]]; then
    ctest --preset "$preset"
fi
(cd "build/$preset" && cpack -B "$dist")
rm -rf "$dist/_CPack_Packages"

if [[ $appimage == 1 ]]; then
    arch="$(uname -m)"
    tools="$dist/.tools"
    mkdir -p "$tools"
    fetch() {
        local name="$1" url="$2"
        if [[ ! -x "$tools/$name" ]]; then
            curl -fL --retry 3 -o "$tools/$name" "$url"
            chmod +x "$tools/$name"
        fi
    }
    fetch "linuxdeploy-$arch.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$arch.AppImage"
    fetch "linuxdeploy-plugin-qt-$arch.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$arch.AppImage"
    appdir="$dist/AppDir"
    rm -rf "$appdir"
    DESTDIR="$appdir" cmake --install "build/$preset"
    export PATH="$tools:$PATH"
    # Libraries from Homebrew on Linux are not on the default search path of linuxdeploy.
    if command -v brew >/dev/null 2>&1; then
        brew_prefix="$(brew --prefix)"
        export LD_LIBRARY_PATH="$brew_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    fi
    export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake || true)}"
    export APPIMAGE_EXTRACT_AND_RUN=1 # works without FUSE (containers, CI)
    (cd "$dist" && OUTPUT="WindTerm-Weifan-$(git -C "$root" describe --tags --always 2>/dev/null || echo dev)-$arch.AppImage" \
        "linuxdeploy-$arch.AppImage" --appdir "$appdir" --plugin qt --output appimage \
        --desktop-file "$appdir/usr/share/applications/windterm-weifan.desktop" \
        --icon-file "$root/app/resources/icons/windterm-weifan-256.png" --icon-filename windterm-weifan)
fi

echo "Packages in $dist:"
for package in "$dist"/*.tar.gz "$dist"/*.deb "$dist"/*.AppImage; do
    if [[ -e "$package" ]]; then
        echo "  $(basename "$package")"
    fi
done
