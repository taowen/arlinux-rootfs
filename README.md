# Arlinux rootfs

Arlinux rootfs is the open Linux side of Arlinux. It cross-builds AArch64 Linux
root filesystems into distribution bundles that can be imported by the Arlinux
Android application. The Android host is intentionally not required to build,
inspect, or extend a distribution.

A bundle contains:

- a distribution-owned root filesystem;
- the bionicx glibc compatibility runtime;
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
  glslang-tools jq libtool meson ninja-build patchelf pkgconf:arm64 python3 tar \
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
./build.sh verify out/debian.arlinux-rootfs
```

Use `./build.sh build` to build every checked-out distribution. Build state is
written to `build/`, finished bundles to `out/`, and reusable downloads to
`${XDG_CACHE_HOME:-$HOME/.cache}/arlinux`. Set `ARLINUX_CACHE_DIR` to move the
cache.

## Create a distribution

A distribution is an independent Git repository placed at
`distributions/<id>`. It does not link against or import the private Android
host. To start a repository alongside the built-in examples:

```bash
git clone https://github.com/you/my-arlinux-distribution.git distributions/my-linux
./build.sh validate distributions/my-linux
./build.sh build my-linux
```

Read [Distribution authoring](docs/DISTRIBUTION-AUTHORING.md) for the complete
contract and a from-scratch walkthrough. Public reference implementations are:

- [arlinux-debian](https://github.com/taowen/arlinux-debian)
- [arlinux-arch](https://github.com/taowen/arlinux-arch)
- [arlinux-omarchy](https://github.com/taowen/arlinux-omarchy)
- [arlinux-lxqt](https://github.com/taowen/arlinux-lxqt)

The on-disk archive contract is documented in [PROTOCOL.md](PROTOCOL.md).
See [Graphics acceleration](docs/GRAPHICS.md) for the default Wayland, X11,
OpenGL, Vulkan, and AHB presentation paths.

## Repository boundaries

Distribution repositories own package selection, rootfs creation, first-boot
configuration, launch profiles, and distribution-specific compatibility
policy. This repository owns the bundle format, glibc bridge, graphics stack,
cross-build tooling, validation, and shared guest support.

Android UI, input, lifecycle, and bundle installation belong to the private
host. A distribution must not depend on host source code or Android build
artifacts.

## License

Arlinux rootfs is GPL-3.0-or-later. Bundled distributions and third-party
submodules retain their own licenses and notices; distributors are responsible
for preserving all applicable notices in binary bundles.
