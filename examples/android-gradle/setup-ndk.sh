#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[[ $(uname -m) == aarch64 ]] || { echo 'This toolchain requires ARM64 Linux.' >&2; exit 1; }
sudo apt-get update
sudo apt-get install -y --no-install-recommends cmake ninja-build xz-utils curl
sdk=${ARLINUX_ANDROID_SDK:-$HOME/.local/share/arlinux/android-sdk}
mkdir -p "$sdk/ndk"
archive="$sdk/android-ndk-r29-aarch64-linux-gnu.tar.xz"
sha=11850c860ba62fc346db5151ee1ac8eb5fada8d95532a9145748799e44bc1b58
if [[ ! -f $archive ]] || ! echo "$sha  $archive" | sha256sum -c --status; then
    curl --fail --location --retry 3 --connect-timeout 20 \
        https://github.com/HomuHomu833/android-ndk-custom/releases/download/r29/android-ndk-r29-aarch64-linux-gnu.tar.xz \
        -o "$archive.part"
    echo "$sha  $archive.part" | sha256sum -c -
    mv "$archive.part" "$archive"
fi
ndk="$sdk/ndk/29.0.14206865"
mkdir -p "$ndk"
tar -xJf "$archive" --strip-components=1 -C "$ndk"
"$ndk/toolchains/llvm/prebuilt/linux-arm64/bin/clang" --version
cmake --version
echo 'ARM64-host NDK r29 ready for Gradle/CMake.'
