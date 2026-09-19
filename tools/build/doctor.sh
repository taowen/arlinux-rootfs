#!/usr/bin/env bash
set -euo pipefail

required=(
  aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ aarch64-linux-gnu-pkg-config
  autoconf automake curl debootstrap file git glslangValidator jq libtoolize
  make meson ninja patchelf python3 readelf sha256sum tar wayland-scanner zstd
)
missing=()
for program in "${required[@]}"; do
  command -v "$program" >/dev/null 2>&1 || missing+=("$program")
done

if [[ ${#missing[@]} -ne 0 ]]; then
  printf 'Missing build tools: %s\n' "${missing[*]}" >&2
  printf 'See README.md for Debian/Ubuntu package names.\n' >&2
  exit 2
fi

[[ "$(uname -s)" == Linux ]] || { echo 'The rootfs build requires Linux.' >&2; exit 2; }
libraries=(egl gl glesv2 wayland-client wayland-egl wayland-server x11 x11-xcb xcb)
missing=()
for library in "${libraries[@]}"; do
  aarch64-linux-gnu-pkg-config --exists "$library" || missing+=("$library")
done
if [[ ${#missing[@]} -ne 0 ]]; then
  printf 'Missing AArch64 development libraries: %s\n' "${missing[*]}" >&2
  printf 'See README.md for Debian/Ubuntu package names.\n' >&2
  exit 2
fi

if [[ "${1:-}" != --quiet ]]; then
  printf 'Build environment is ready (%s, %s).\n' "$(uname -s)" "$(uname -m)"
fi
