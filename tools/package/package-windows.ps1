<#
.SYNOPSIS
  Builds and packages WindTerm-Weifan for Windows x64 (ZIP, plus an NSIS installer if makensis
  is installed).

.DESCRIPTION
  Run from a "x64 Native Tools" developer prompt (MSVC + Ninja on PATH) with:
    $env:VCPKG_ROOT         vcpkg checkout (libssh2/OpenSSL come from vcpkg.json)
    $env:CMAKE_PREFIX_PATH  Qt 6 msvc2022_64 directory, e.g. C:\Qt\6.8.2\msvc2022_64
  Optional code signing:
    $env:WT_SIGN_CERT_THUMBPRINT  certificate thumbprint in the user's store for signtool
    $env:WT_SIGN_TIMESTAMP_URL    RFC 3161 timestamp server (default http://timestamp.digicert.com)

.EXAMPLE
  pwsh tools/package/package-windows.ps1 -SkipTests
#>
param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$dist = if ($env:WT_DIST_DIR) { $env:WT_DIST_DIR } else { Join-Path $root 'dist' }
$preset = 'windows-msvc'
Set-Location $root
New-Item -ItemType Directory -Force -Path $dist | Out-Null

cmake --preset $preset -DWT_WARNINGS_AS_ERRORS=ON
if ($LASTEXITCODE) { throw 'configure failed' }
cmake --build --preset $preset
if ($LASTEXITCODE) { throw 'build failed' }
if (-not $SkipTests) {
    ctest --preset $preset
    if ($LASTEXITCODE) { throw 'tests failed' }
}

$build = Join-Path $root "build\$preset"
$stage = Join-Path $dist 'WindTerm-Weifan'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
cmake --install $build --prefix $stage
if ($LASTEXITCODE) { throw 'install failed' }
$exe = Join-Path $stage 'bin\WindTerm-Weifan.exe'

# Qt runtime and plugins, then the vcpkg DLLs (libssh2, libcrypto) copied next to the build output.
$windeployqt = Get-Command windeployqt -ErrorAction SilentlyContinue
if (-not $windeployqt -and $env:CMAKE_PREFIX_PATH) {
    $windeployqt = Join-Path $env:CMAKE_PREFIX_PATH 'bin\windeployqt.exe'
}
& $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw $exe
if ($LASTEXITCODE) { throw 'windeployqt failed' }
Get-ChildItem (Join-Path $build 'app') -Filter *.dll | Copy-Item -Destination (Join-Path $stage 'bin') -Force

if ($env:WT_SIGN_CERT_THUMBPRINT) {
    $timestamp = if ($env:WT_SIGN_TIMESTAMP_URL) { $env:WT_SIGN_TIMESTAMP_URL } else { 'http://timestamp.digicert.com' }
    Get-ChildItem $stage -Recurse -Include *.exe, *.dll | ForEach-Object {
        signtool sign /sha1 $env:WT_SIGN_CERT_THUMBPRINT /fd SHA256 /tr $timestamp /td SHA256 $_.FullName
        if ($LASTEXITCODE) { throw "signing $($_.Name) failed" }
    }
}

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern '^\s+VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$zip = Join-Path $dist "WindTerm-Weifan-$version-Windows-x64.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip
if (Get-Command makensis -ErrorAction SilentlyContinue) {
    Push-Location $build
    cpack -G NSIS -B $dist
    Pop-Location
}
Write-Host "Created $zip"
