# Kotlin Android development on the phone

A normal Android Gradle project, edited in Geany and compiled by the phone's
ARM64 JVM. Its APK runs in an ARLinux desktop window without Android system
installation. The sample covers two Activities, a dialog and a saved Unicode
note, a Material 3 Compose editor and dialog, and Kotlin calling a C++ JNI
library. Both UI toolkits use the
same hosted Android runtime and accessibility/input path.

Copy this directory to `~/Projects/android-gradle` in Debian or Yibu, then:

```sh
bash setup.sh
geany studio.geany app/src/main/java/dev/arlinux/pocketstudio/MainActivity.kt
```

Use **Build > Build and run APK**, or `bash run.sh`. To rebuild offline once
dependencies are cached, use `bash build.sh --offline`. A clean offline build is
`bash build.sh clean --offline`. The standard signed APK is at
`app/build/outputs/apk/debug/app-debug.apk`:

```sh
arlinux-app --apk app/build/outputs/apk/debug/app-debug.apk
```

Rebuilding and opening the same package replaces its test process, loads new
code and preserves private test data. Use a package name not installed on the
phone. This does not require ADB, root, an emulator or a computer. Debug keys
are generated locally by Gradle; use your own protected key for production.

## Build tools

The example pins AGP 8.5.2, Gradle 8.7, Kotlin 1.9.24, Compose compiler 1.5.14
and SDK 34, with NDK r29 (29.0.14206865) and CMake 3.31.6. Debian supplies
OpenJDK and the native ARM64 AAPT2. Google supplies platform data and build-tool
data; Gradle performs Kotlin compilation, D8 dexing, ZIP packaging and signing
in Java. `android.aapt2FromMavenOverride` selects the native AAPT2 instead of
Google's x86-64 Maven executable. AGP labels this setting experimental.

The official Linux NDK host executables are x86-64. Setup downloads the pinned,
SHA-256-verified [ARM64 GNU/Linux NDK r29 build](https://github.com/HomuHomu833/android-ndk-custom/releases/tag/r29)
by HomuHomu833 instead. This is a community toolchain, not Google's official
Linux binary release. It uses Android's NDK sysroot and standard CMake toolchain
to build Bionic `arm64-v8a` libraries, not glibc libraries. Gradle packages
`libpocketnative.so` and `libc++_shared.so`; `System.loadLibrary` loads the former,
and the main screen displays the result calculated by C++. Android `liblog`
receives the same result. C++17 containers exercise the real C++ runtime.

NDK setup is included in `setup.sh`. To add it to an existing copy, run
`bash setup-ndk.sh`. The NDK is installed under the SDK's `ndk/29.0.14206865`
(the same SDK location used by `build.sh`), so Gradle uses ordinary `ndkVersion`
selection. Debian's ARM64 CMake and Ninja execute natively on the phone; no
emulated x86 build tools are needed. Edit `app/src/main/cpp/native.cpp`, then use
the same **Build and run APK** command to reload changed native code.

Setup verifies SDK/Gradle archive hashes and refreshes Debian's Java CA store.
It needs network access to Debian's configured mirror, Google's SDK/Maven
repository, Maven Central, Gradle's distribution service and GitHub for the
ARM64-host NDK. Google SDK
downloads are subject to its SDK terms. The first Gradle build caches its Maven
dependencies; adding a new dependency may require network access again.

The SDK's official Linux native executables other than the overridden AAPT2
are x86-64. This example does not execute them. AIDL,
installation-dependent services and split APK sets are not validated here.
AGP may warn that platform-tools are absent: `assembleDebug` and the hosted
launcher do not need ADB, and this setup does not fabricate an ADB executable
or accept its installation license on your behalf. Geany is a lightweight
editor with build commands, not Android Studio's previews or debugger.
Native testing here covers ARM64 JNI, shared libc++, Android liblog and
changed-code reload; it does not imply coverage of every NDK API, ABI or debugger.

For the smallest Java-only example without Gradle, see
[android-dev](../android-dev).
