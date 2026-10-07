#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
sdk=${ARLINUX_ANDROID_SDK:-$HOME/.local/share/arlinux/android-sdk}
[[ -f $sdk/android.jar && -f $sdk/r8.jar ]] || { echo 'Run setup.sh first.' >&2; exit 1; }
mkdir -p build/{generated,classes,dex}
# A fresh compiler output avoids retaining classes that have been removed from source.
find build/classes build/generated build/dex -type f -delete
aapt package -f -m -J build/generated -M AndroidManifest.xml -S res -I "$sdk/android.jar" -F build/resources.apk
find src build/generated -name '*.java' -print > build/sources.list
javac --release 8 -classpath "$sdk/android.jar" -d build/classes @build/sources.list
jar cf build/classes.jar -C build/classes .
java -Xmx256m -cp "$sdk/r8.jar" com.android.tools.r8.D8 --min-api 28 --lib "$sdk/android.jar" --output build/dex build/classes.jar
cp build/resources.apk build/unsigned.apk
(cd build/dex && zip -q -j ../unsigned.apk ./*.dex)
if [[ ! -f $sdk/debug.p12 ]]; then
    keytool -genkeypair -keystore "$sdk/debug.p12" -storetype PKCS12 -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=Android Debug,O=ARLinux,C=US'
fi
apksigner sign --ks "$sdk/debug.p12" --ks-pass pass:android --out build/app.apk build/unsigned.apk
apksigner verify build/app.apk
echo "Built $PWD/build/app.apk on $(uname -m)"
if [[ ${1:-} = --run ]]; then arlinux-app --apk "$PWD/build/app.apk"; fi
