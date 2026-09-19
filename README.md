# Arlinux rootfs

This is a new repository for Linux-side Arlinux components. Its Git history
starts here, independently of the Android host. Debian, Arch, and Omarchy are
submodules under `distributions/`; Mesa, libhybris, and Android headers are
pinned Linux build inputs. Android UI, JNI, compositor binaries and Android
build files stay in [arlinux](https://github.com/taowen/arlinux).

`./build.sh build debian arch omarchy` builds three independent `.arlinux-rootfs`
bundles in WSL. `python3 tools/bundle.py verify out/debian.arlinux-rootfs`
checks the complete bundle without installing it. All generated outputs are
ignored. The glibc and GPU paths target the single Android host package
`io.taowen.arlinux`, not a distribution-specific APK.

The build command initializes the selected distributions and their nested
submodules, so a fresh checkout does not need a separate recursive setup step.

See [protocol v1](PROTOCOL.md) for the manifest, instance layout and update
rules. A bundle is not an APK. Android owns the surface, input, hosted apps and
compositor; Linux owns package management and the desktop process.
