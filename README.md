# Arlinux rootfs

Arlinux rootfs builds portable Linux distribution bundles for the Arlinux
Android host. This repository contains only Linux code and uses ordinary Bash
and Linux tools. A bundle is not an APK: it contains a root filesystem, the
shared glibc compatibility runtime and the Qualcomm userspace GPU stack.

## Build

Builds run on an x86-64 Linux machine and produce AArch64 bundles. On a fresh
Debian or Ubuntu installation, enable the arm64 architecture and install the
toolchain:

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

Then clone and build:

```bash
git clone --recurse-submodules https://github.com/taowen/arlinux-rootfs.git
cd arlinux-rootfs
./build.sh doctor
./build.sh build                 # all distributions
./build.sh build debian arch     # selected distributions
./build.sh verify out/debian.arlinux-rootfs
```

Build state stays under `build/`, reusable downloads and source builds under
`${XDG_CACHE_HOME:-$HOME/.cache}/arlinux`, distribution assets under each
distribution's ignored `build/` directory, and finished bundles under `out/`.
Set `ARLINUX_CACHE_DIR` to place the shared cache elsewhere. The build itself
does not require root privileges.

## Add a distribution

Place a repository or submodule at `distributions/<id>`. The ID must be lower
case and may contain digits and hyphens. It needs only:

- `product.json`: display name, glibc version, compositor hint, library paths,
  required files and optional launch environment;
- `tools/seed.sh OUTPUT`: create an AArch64 root filesystem at `OUTPUT`;
- `guest/`: files installed at `/usr/lib/arlinux/guest`;
- `native/product-policy.h`: distribution-specific compatibility policy;
- optional `profile.json` and `rootfs.lock.json` for launch and reproducibility.

`./build.sh list` discovers distributions from their `product.json`; the build
contains no hard-coded distribution list. Start by copying the structure of the
closest existing distribution and keep host UI or device automation out of the
distribution repository.

See [PROTOCOL.md](PROTOCOL.md) for the bundle contract. Android UI, input,
compositors and installation live in the separate Arlinux host repository.
