#!/usr/bin/env bash
# Build an isolated x64 WebView2 host with LLVM-MinGW and the official SDK.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${BUILD_DIR:-$here/build}"
mkdir -p "$build"
version=1.0.3485.44
sdk="$build/sdk"
if [[ ! -f "$sdk/build/native/include/WebView2.h" ]]; then
    curl --fail --location --retry 3 \
        "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$version/microsoft.web.webview2.$version.nupkg" \
        -o "$build/webview2.nupkg"
    mkdir -p "$sdk"
    unzip -q -o "$build/webview2.nupkg" -d "$sdk"
fi
# The SDK uses Windows filename casing; LLVM-MinGW's header is lowercase.
printf '#include <eventtoken.h>\n' > "$build/EventToken.h"
"${CXX:-x86_64-w64-mingw32-clang++}" -fms-extensions -std=c++17 -static -municode \
    -I"$build" -I"$sdk/build/native/include" "$here/main.cpp" \
    -lole32 -luuid -o "$build/webview-probe.exe"
cp "$sdk/runtimes/win-x64/native/WebView2Loader.dll" "$build/"
echo "Built $build/webview-probe.exe"
