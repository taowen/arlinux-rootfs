#!/bin/sh
set -eu
# The Android GPU overlay supplies this package's matching Mesa providers.
# Keep rolling repository upgrades from replacing them with desktop drivers.
if ! grep -q '^IgnorePkg = mesa$' /etc/pacman.conf; then
    sed -i '/^\[options\]/a IgnorePkg = mesa' /etc/pacman.conf
fi
archive=/usr/lib/arlinux/mesa-packages/mesa-aarch64.pkg.tar.zst
if [ -f "$archive" ]; then
    echo 'ARLINUX:Installing the shared Android Mesa providers...'
    pacman -U --needed --noconfirm \
        --overwrite 'usr/lib/mesa/*,usr/share/glvnd/egl_vendor.d/50_mesa.json' "$archive"
    rm -f "$archive"
fi
case "$(pacman -Q mesa)" in
    *arlinux*) ;;
    *) echo 'Missing the bundled Android Mesa provider' >&2; exit 1 ;;
esac
# Remove only unused dependencies of the replaced desktop Mesa package.
# Keep LLVM if an installed compiler or application still needs it.
orphans=$(pacman -Qdtq 2>/dev/null | grep -E '^(llvm-libs|z3)$' || true)
if [ -n "$orphans" ]; then
    pacman -Rns --noconfirm $orphans
fi
