#!/usr/bin/env bash
set -euo pipefail
sudo apt-get update
sudo apt-get install -y --no-install-recommends default-jdk-headless aapt apksigner zip curl unzip geany debianutils
sdk=${ARLINUX_ANDROID_SDK:-$HOME/.local/share/arlinux/android-sdk}
mkdir -p "$sdk"
download() {
    local url=$1 sha=$2 target=$3
    if [[ ! -f $target ]] || ! echo "$sha  $target" | sha256sum -c --status; then
        curl --fail --location --retry 3 --connect-timeout 20 "$url" -o "$target.part"
        echo "$sha  $target.part" | sha256sum -c -
        mv "$target.part" "$target"
    fi
}
# Platform SDK data and D8 are architecture-independent. Native build programs
# come from Debian's ARM64 packages, not Google's x86-only Linux build tools.
download https://dl.google.com/android/repository/platform-34-ext7_r03.zip \
    16fdb74c55e59ae3ef52def135aec713508467bd56d7dabcd8c9be31fa8b20f3 "$sdk/platform.zip"
download https://dl.google.com/dl/android/maven2/com/android/tools/r8/8.3.37/r8-8.3.37.jar \
    59753e70a74f918389cc87f1b7d66b5c0862932559167425708ded159e3de439 "$sdk/r8.jar"
entry=$(unzip -Z1 "$sdk/platform.zip" | awk '/\/android.jar$/ && !found { print; found=1 }')
[[ -n $entry ]] || { echo 'Platform archive has no android.jar' >&2; exit 1; }
unzip -p "$sdk/platform.zip" "$entry" > "$sdk/android.jar"
echo 'Ready. Open workbench.geany in Geany; Build > Build and run APK.'
