#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
sudo apt-get update
sudo apt-get install -y --no-install-recommends default-jdk-headless aapt ca-certificates-java curl unzip geany
# Debian's Java trust store must be generated after the JRE is available.
sudo update-ca-certificates --fresh
sdk=${ARLINUX_ANDROID_SDK:-$HOME/.local/share/arlinux/android-sdk}
mkdir -p "$sdk/platforms" "$sdk/build-tools"
download() {
    local url=$1 sha=$2 target=$3 algorithm=${4:-sha256sum}
    if [[ ! -f $target ]] || ! echo "$sha  $target" | "$algorithm" -c --status; then
        curl --fail --location --retry 3 --connect-timeout 20 "$url" -o "$target.part"
        echo "$sha  $target.part" | "$algorithm" -c -
        mv "$target.part" "$target"
    fi
}
download https://downloads.gradle.org/distributions/gradle-8.7-bin.zip \
    544c35d6bd849ae8a5ed0bcea39ba677dc40f49df7d1835561582da2009b961d "$sdk/gradle.zip"
download https://dl.google.com/android/repository/platform-34-ext7_r03.zip \
    16fdb74c55e59ae3ef52def135aec713508467bd56d7dabcd8c9be31fa8b20f3 "$sdk/platform.zip"
# SHA-1 is the archive checksum published in Google's SDK repository metadata.
download https://dl.google.com/android/repository/build-tools_r34-linux.zip \
    d6d58e0c6925a9e4d9a541e84cd1f405c2f9d2a9 "$sdk/build-tools.zip" sha1sum
unzip -oq "$sdk/gradle.zip" -d "$sdk"
staging=$(mktemp -d "$sdk/extract.XXXXXX")
trap 'rmdir "$staging" 2>/dev/null || true' EXIT
unzip -oq "$sdk/platform.zip" -d "$staging"
platform=$(find "$staging" -name android.jar -print -quit)
[[ -n $platform ]] || { echo 'Platform archive has no android.jar' >&2; exit 1; }
mkdir -p "$sdk/platforms/android-34"
cp -a "$(dirname "$platform")/." "$sdk/platforms/android-34/"
unzip -oq "$sdk/build-tools.zip" -d "$staging"
mkdir -p "$sdk/build-tools/34.0.0"
cp -a "$staging/android-14/." "$sdk/build-tools/34.0.0/"
# Only extracted copies in this newly created, validated staging directory.
find "$staging" -depth -delete
trap - EXIT
bash setup-ndk.sh
bash build.sh --console=plain
echo 'Ready. Open studio.geany; Build > Build and run APK.'
