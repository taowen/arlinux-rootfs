# Android development on the phone

This small Java project builds a standard signed Android APK with Geany and
native ARM64 Debian tools. It does not require Android Studio, an emulator,
ADB, root, or a desktop computer to build and run.

In a Debian or Yibu instance, copy this directory into your home directory:

```sh
bash setup.sh
geany workbench.geany src/dev/arlinux/workbench/MainActivity.java
```

Use **Build > Build APK** or **Build > Build and run APK**. The equivalent
terminal command is `bash build.sh --run`.

Setup installs Debian's Java compiler, AAPT, APK signer and Geany. The platform
SDK data and D8 compiler are downloaded from Google and checked against pinned
SHA-256 hashes. Setup needs network access; subsequent builds and local test
runs work offline. Google SDK downloads are subject to its SDK terms.

The build produces `build/app.apk`. To run any supported standalone signed APK:

```sh
arlinux-app --apk ./build/app.apk
```

ARLinux snapshots the APK into its private Android runtime and opens a desktop
window. It does **not** install the package into Android's system application
list or show an installation confirmation. Rebuilding and running the same
package closes its old test process, loads the new code and keeps test data.
Use a development package name distinct from an app installed on the phone.

This is a lightweight Java/framework starting point, not a replacement for the
Android Gradle plugin's complete dependency, Compose, Kotlin and NDK workflow.
For a standard Kotlin/Gradle project using the same launcher, see
[android-gradle](../android-gradle).
The launcher accepts standalone APKs, not split APK sets or AABs. Hosted tasks
currently support same-package/same-process Activity navigation; system-level
integrations and installation-dependent services still need separate validation.
Production signing must use your own protected key, not this development key.
