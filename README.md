# Arlinux rootfs

Arlinux rootfs is the open Linux side of Arlinux. It cross-builds AArch64 Linux
root filesystems into distribution bundles that can be imported by the Arlinux
Android application. The Android host is intentionally not required to build,
inspect, or extend a distribution.

A bundle contains:

- a distribution-owned root filesystem;
- distribution-native glibc and loader, executed by the host's tawcroot runtime;
- a Qualcomm Turnip/Zink graphics overlay;
- a launch profile and a versioned manifest.

The project is Linux-native. Its build and distribution interfaces use Bash,
Python, and standard Linux tools; there are no Windows or PowerShell build
paths in this repository or its distribution repositories.

## Build an existing distribution

Builds run without root privileges on an x86-64 Debian or Ubuntu host and
produce AArch64 bundles. Install the toolchain once:

```bash
sudo dpkg --add-architecture arm64
sudo apt update
sudo apt install \
  autoconf automake binutils-aarch64-linux-gnu build-essential ca-certificates \
  curl debootstrap file g++-aarch64-linux-gnu gcc-aarch64-linux-gnu git \
  glslang-tools jq libarchive-tools libtool meson ninja-build patchelf pkgconf:arm64 python3 tar \
  wayland-protocols zstd \
  libegl-dev:arm64 libgl-dev:arm64 libgles-dev:arm64 libwayland-dev:arm64 \
  libx11-dev:arm64 libx11-xcb-dev:arm64 libxcb1-dev:arm64
```

Clone and build:

```bash
git clone --recurse-submodules https://github.com/taowen/arlinux-rootfs.git
cd arlinux-rootfs
./build.sh doctor
./build.sh list
./build.sh build debian
./build.sh verify out/debian.zip
```

Use `./build.sh build` to build every checked-out distribution. Build state is
written to `build/`, finished bundles to `out/`, and reusable downloads to
`${XDG_CACHE_HOME:-$HOME/.cache}/arlinux`. Set `ARLINUX_CACHE_DIR` to move the
cache.

Prebuilt ZIP bundles are published as release assets in each distribution's
own GitHub repository: [Debian](https://github.com/taowen/arlinux-debian/releases),
[Arch](https://github.com/taowen/arlinux-arch/releases),
[Omarchy](https://github.com/taowen/arlinux-omarchy/releases), and
[LXQt](https://github.com/taowen/arlinux-lxqt/releases). Download the `.zip`
asset, not GitHub's automatic source-code archive. The Android APK is released
separately; a rootfs bundle is imported after installing it.

## Create a distribution

A distribution is an independent Git repository placed at
`distributions/<id>`. It does not link against or import the private Android
host. To start a repository alongside the built-in examples:

```bash
git clone https://github.com/you/my-arlinux-distribution.git distributions/my-linux
./build.sh validate distributions/my-linux
./build.sh build my-linux
```

Public reference implementations are:

- [arlinux-debian](https://github.com/taowen/arlinux-debian)
- [arlinux-arch](https://github.com/taowen/arlinux-arch)
- [arlinux-omarchy](https://github.com/taowen/arlinux-omarchy)
- [arlinux-lxqt](https://github.com/taowen/arlinux-lxqt)

Maintainers can publish one distribution from a clean checkout with
`tools/publish-distribution.sh <id> <tag>` after authenticating `gh`. It builds
and verifies the bundle, then creates a release in that distribution's GitHub
repository with the ZIP and its SHA-256 in the release notes.

## Documentation

- [Distribution authoring](docs/DISTRIBUTION-AUTHORING.md): layout, build steps,
  and desktop integration for a new rootfs.
- [Static bundle protocol](docs/STATIC-PROTOCOL.md): required archive contents,
  installation, instances, and first boot.
- [Android–Linux runtime protocol](docs/RUNTIME-PROTOCOL.md): the live session,
  display, input, graphics, audio, and accessibility contract.
- [Graphics acceleration](docs/GRAPHICS.md): how Vulkan, OpenGL, Wayland, X11,
  and AHB fit together, including current limitations.

The [graphics protocol package](graphics-protocols/README.md) documents wire
definitions for Xwayland, Mesa, libhybris, and compositor developers. The
The Android host uses [tawcroot](https://github.com/taowen/tawc/tree/main/tawcroot)
for syscall compatibility; distribution libc packages remain unmodified.

## Repository boundaries

Distribution repositories own package selection, rootfs creation, first-boot
configuration, launch profiles, and distribution-specific compatibility
policy. This repository owns the bundle format, guest integration, graphics stack,
cross-build tooling, validation, and shared guest support.

Android UI, input, lifecycle, and bundle installation belong to the private
host. A distribution must not depend on host source code or Android build
artifacts.

## License

Arlinux rootfs is GPL-3.0-or-later. Bundled distributions and third-party
submodules retain their own licenses and notices; distributors are responsible
for preserving all applicable notices in binary bundles.
