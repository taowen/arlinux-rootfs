#!/usr/bin/env bash
# Build the shared runtime and selected distribution bundles on Linux.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo"
export ARLINUX_CACHE_DIR="${ARLINUX_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/arlinux}"
rebuild=false
if [[ "${1:-}" == --rebuild ]]; then rebuild=true; shift; fi
products=("$@")
if [[ ${#products[@]} -eq 0 ]]; then
    mapfile -t products < <(find distributions -mindepth 2 -maxdepth 2 \
        -name product.json -printf '%h\n' | sed 's|.*/||' | sort)
fi
gpu_inputs="$(git submodule status third_party/mesa third_party/libhybris third_party/android-headers)"
gpu_inputs+="$(git -C third_party/mesa diff --binary HEAD | sha256sum)"

./tools/build/doctor.sh --quiet

# Runtime changes do not require downloading and unpacking the distribution
# again. Cache only the unmodified seed, before adding libc or GPU payloads.
seed_rootfs() (
    product="$1"; destination="$2"
    [[ "$product" =~ ^[a-z0-9][a-z0-9-]*$ ]] || exit 2
    product_dir="$repo/distributions/$product"
    seed_id="$(cd "$product_dir" && {
        sha256sum product.json rootfs.lock.json
        find tools guest -type f -not -path '*/__pycache__/*' -print0 |
            sort -z | xargs -0 -r sha256sum
    } | sha256sum | cut -c1-24)"
    base="$(realpath -m "$ARLINUX_CACHE_DIR/seeds")"
    mkdir -p "$base"
    entry="$base/$product-$seed_id"
    exec 9>"$entry.lock"
    flock 9
    if [[ "$rebuild" == true || ! -f "$entry/complete" ]]; then
        temporary="$(mktemp -d "$entry.XXXXXXXX")"
        trap 'rm -rf -- "$temporary"' EXIT
        "$product_dir/tools/seed.sh" "$temporary/rootfs"
        if [[ -x "$product_dir/tools/post-seed.sh" ]]; then
            "$product_dir/tools/post-seed.sh" "$temporary/rootfs"
        fi
        touch "$temporary/complete"
        # This is one validated, content-addressed cache entry, not a guest
        # instance or a user-supplied recursive deletion target.
        rm -rf -- "$entry"
        mv "$temporary" "$entry"
        trap - EXIT
    else
        echo "OK seed    $product (cached)"
    fi
    cp -a --reflink=auto "$entry/rootfs/." "$destination/"
)

input_id() {
    local product="$1"
    local inputs=(runtime graphics-protocols examples tools/build tools/arlinux-app-data)
    {
        git ls-files -s -- "${inputs[@]}"
        git diff --binary -- "${inputs[@]}"
        git ls-files --others --exclude-standard -z -- \
            "${inputs[@]}" \
            | sort -z | xargs -0 -r sha256sum
        git -C "distributions/$product" ls-files -s guest tools \
            product.json profile.json rootfs.lock.json
        git -C "distributions/$product" diff --binary -- guest tools \
            product.json profile.json rootfs.lock.json
        (cd "distributions/$product" &&
            git ls-files --others --exclude-standard -z -- \
                guest tools product.json profile.json rootfs.lock.json \
                | sort -z | xargs -0 -r sha256sum)
        git -C "distributions/$product" submodule status --recursive || true
        printf '%s\n' "$gpu_inputs"
        true
    } | sha256sum | cut -d' ' -f1
}

pending=()
declare -A ids
for product in "${products[@]}"; do
    [[ -f "distributions/$product/product.json" ]] || { echo "Unknown product: $product" >&2; exit 2; }
    assets="distributions/$product/build/assets"
    ids[$product]="$(input_id "$product")"
    if [[ "$rebuild" == true || ! -s "$assets/rootfs.tar.zst" || ! -s "$assets/gpu-qualcomm.tar.zst" \
          || ! -s "$assets/gpu-generic.tar.zst" || ! -s "$assets/gpu-generic-id" \
          || "$(cat "$assets/.inputs.sha256" 2>/dev/null || true)" != "${ids[$product]}" ]]; then
        pending+=("$product")
    else
        echo "OK assets  $product (cached)"
    fi
done
[[ ${#pending[@]} -gt 0 ]] || exit 0

echo '== Linux runtime and examples =='
for product in "${pending[@]}"; do
    output="build/linux/runtime/$product"; mkdir -p "$output"
    aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror runtime/tools/guest-sudo.c -o "$output/sudo"
    aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror runtime/tools/bwrap-compat.c -o "$output/bwrap"
done
teapot=build/linux/teapot; mkdir -p "$teapot"
xml=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
wayland-scanner client-header "$xml" "$teapot/xdg-shell-client-protocol.h"
wayland-scanner private-code "$xml" "$teapot/xdg-shell-protocol.c"
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -idirafter /usr/include \
  examples/teapot/teapot-glx.c examples/teapot-common/utah_teapot.c \
  examples/teapot-common/teapot_gate.c -Iexamples/teapot-common -lm -lX11 -lGL -o "$teapot/teapot-glx"
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -Wno-unused-parameter -idirafter /usr/include \
  examples/teapot/teapot-egl.c examples/teapot-common/utah_teapot.c \
  examples/teapot-common/teapot_gate.c "$teapot/xdg-shell-protocol.c" \
  -Iexamples/teapot-common -I"$teapot" -lm -lwayland-client -lwayland-egl -lEGL -lGLESv2 \
  -o "$teapot/teapot-egl"

echo '== Linux GPU stack =='
"$repo/tools/build/linux-gpu.sh"
mesa="$repo/build/linux/mesa/lib"
hybris="$repo/build/linux/libhybris/install/usr/lib/hybris"
python3 "$repo/tools/build/mesa-debs.py"
python3 "$repo/tools/build/mesa-arch.py"

# Every GPU library finds its neighbours relative to its own location. The
# exact Android app-private directory is supplied only when the guest runs.
set_relative_rpath() {
    local tree="$1" library="$2" origin entry rpath="" relative
    origin="$(dirname "$library")"
    for entry in usr/lib/mesa usr/lib lib \
                 usr/lib/hybris usr/lib/hybris/libhybris; do
        relative="$(realpath -m --relative-to="$origin" "$tree/$entry")"
        rpath+="${rpath:+:}\$ORIGIN/$relative"
    done
    patchelf --set-rpath "$rpath" "$library"
}

echo '== Product root filesystems =='
stage="${ARLINUX_PRODUCT_STAGE:-$ARLINUX_CACHE_DIR/products}"; mkdir -p "$stage"
for product in "${pending[@]}"; do
    product_dir="$repo/distributions/$product"; rootfs="$stage/$product/rootfs"
    assets="$product_dir/build/assets"
    rm -rf "$stage/$product" "$assets"; mkdir -p "$rootfs" "$assets"
    seed_rootfs "$product" "$rootfs"
    python3 - "$product_dir/product.json" "$rootfs" <<'PY'
import json, os, pathlib, sys
product = json.loads(pathlib.Path(sys.argv[1]).read_text())
rootfs = pathlib.Path(sys.argv[2])
missing = [path for path in product['requiredFiles']
           if not os.path.lexists(rootfs / path)]
if missing:
    raise SystemExit('rootfs seed is missing required files: ' + ', '.join(missing))
PY
    mkdir -p "$rootfs/usr/lib/arlinux/guest"
    install -Dm644 runtime/tools/steam.py "$rootfs/usr/lib/arlinux/steam.py"
    install -Dm644 runtime/tools/steam_fex.py "$rootfs/usr/lib/arlinux/steam_fex.py"
    install -Dm755 runtime/tools/arlinux-steam "$rootfs/usr/bin/arlinux-steam"
    install -Dm644 runtime/tools/arlinux-steam.desktop "$rootfs/usr/share/applications/arlinux-steam.desktop"
    install -Dm644 runtime/tools/steam.png "$rootfs/usr/share/pixmaps/arlinux-steam.png"
    install -Dm644 runtime/tools/opencode.py "$rootfs/usr/lib/arlinux/opencode.py"
    install -Dm755 runtime/tools/arlinux-opencode "$rootfs/usr/bin/arlinux-opencode"
    install -Dm644 runtime/tools/arlinux-opencode.desktop "$rootfs/usr/share/applications/arlinux-opencode.desktop"
    install -Dm644 runtime/tools/opencode.png "$rootfs/usr/share/pixmaps/arlinux-opencode.png"
    install -Dm755 "build/linux/runtime/$product/bwrap" "$rootfs/usr/local/bin/bwrap"
    cp tools/arlinux-app-data/hosted-ime.py tools/arlinux-app-data/org.arlinux.HostedInput.service \
      "$rootfs/usr/lib/arlinux/"
    install -Dm644 tools/arlinux-app-data/org.arlinux.HostedInput.service \
      "$rootfs/usr/share/dbus-1/services/org.arlinux.HostedInput.service"
    cp -a "$product_dir/guest/." "$rootfs/usr/lib/arlinux/guest/"
    if [[ "$product" == yibu || "$product" == debian ]]; then
        mkdir -p "$rootfs/usr/lib/arlinux/mesa-packages"
        cp build/linux/mesa-debs/*.deb "$rootfs/usr/lib/arlinux/mesa-packages/"
    elif [[ "$product" == arch || "$product" == omarchy ]]; then
        mkdir -p "$rootfs/usr/lib/arlinux/mesa-packages"
        cp build/linux/mesa-arch/*.pkg.tar.zst "$rootfs/usr/lib/arlinux/mesa-packages/"
        install -Dm755 runtime/tools/install-mesa-arch.sh "$rootfs/usr/lib/arlinux/install-mesa-arch.sh"
    fi
    cp examples/desk-auto/dump-atspi.py examples/desk-auto/atspi-do.py \
      "$rootfs/usr/lib/arlinux/guest/"
    chmod 755 "$rootfs/usr/lib/arlinux/guest/"*.py \
      "$rootfs/usr/lib/arlinux/guest/"*.sh
    [[ ! -f "$rootfs/usr/lib/arlinux/guest/arlinux-a11y" ]] ||
      chmod 755 "$rootfs/usr/lib/arlinux/guest/arlinux-a11y"
    python3 - "$rootfs" <<'PY'
import os, pathlib, sys
root = pathlib.Path(sys.argv[1])
# Keep package-owned shebangs and symlink contents unchanged. tawcroot
# resolves Linux paths; rewriting files here creates a second path policy.
for name, contents in [('etc/resolv.conf','nameserver 1.1.1.1\n'), ('etc/machine-id','')]:
    path = root / name
    if path.is_symlink(): path.unlink()
    path.parent.mkdir(parents=True, exist_ok=True); path.write_text(contents)
# An unprivileged guest observes its process mount table through procfs.
mtab = root / 'etc/mtab'
if not os.path.lexists(mtab):
    mtab.symlink_to('/proc/self/mounts')
PY
    mkdir -p "$assets/bionicx" "$assets/arlinux"
    if [[ -f "$rootfs/usr/share/arlinux/offline-desktop" ]]; then
        cp "$rootfs/usr/share/arlinux/offline-desktop" "$assets/offline-desktop"
    fi
    cp -a "$product_dir/guest" "$assets/guest"
    cp examples/desk-auto/dump-atspi.py examples/desk-auto/atspi-do.py "$assets/guest/"
    chmod 755 "$assets/guest/"*.py "$assets/guest/"*.sh
    [[ ! -f "$assets/guest/arlinux-a11y" ]] ||
      chmod 755 "$assets/guest/arlinux-a11y"
    cp "build/linux/runtime/$product/sudo" "$assets/bionicx/sudo"
    cp "$teapot/teapot-glx" "$teapot/teapot-egl" "$assets/arlinux/"
    cp tools/arlinux-app-data/accessibility-session.sh "$assets/arlinux/"
    cp -a tools/arlinux-app-data/fonts "$assets/arlinux/fonts"
    python3 - "$product_dir" "$assets" <<'PY'
import json, pathlib, sys
product, assets = map(pathlib.Path, sys.argv[1:])
config = json.loads((product/'product.json').read_text())
profile_file = product/'profile.json'
profile = json.loads(profile_file.read_text()); profile['launch']['environment'].update(config.get('environment',{}))
(assets/'profile.json').write_text(json.dumps(profile,indent=2)+'\n')
(assets/'guest.properties').write_text('distributionId='+product.name+'\nrequiredFiles='+','.join(config['requiredFiles'])+'\nlibraryDirectories='+','.join(config['libraryDirectories'])+'\n')
PY
    tar -C "$rootfs" --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
      --exclude=./proc --exclude=./sys --exclude=./dev --exclude=./run --exclude=./tmp \
      --exclude=./var/run --exclude=./var/lock -cf - . | zstd -T4 -19 -f -o "$assets/rootfs.tar.zst"
    sha256sum "$assets/rootfs.tar.zst" | cut -d' ' -f1 > "$assets/rootfs-seed-id"

    overlay="$stage/$product/gpu"
    mkdir -p "$overlay/usr/lib/mesa/dri" "$overlay/usr/lib/mesa/gbm" \
      "$overlay/usr/lib/arlinux/vulkan" "$overlay/usr/share/vulkan/icd.d" \
      "$overlay/usr/share/glvnd/egl_vendor.d"
    cp -a "$mesa"/libEGL.so* "$mesa"/libGLESv2.so* "$mesa"/libGL.so* "$mesa"/libgallium-*.so \
      "$mesa/libvulkan_freedreno.so" "$mesa/libvulkan.so.1" "$overlay/usr/lib/mesa/"
    cp -a "$mesa"/libEGL_mesa.so* "$mesa"/libGLX_mesa.so* "$mesa"/libgbm.so* \
      "$mesa/libGLX.so.0" "$mesa/libGLdispatch.so.0" "$mesa/libOpenGL.so.0" "$overlay/usr/lib/mesa/"
    cp -a "$mesa/gbm/dri_gbm.so" "$overlay/usr/lib/mesa/gbm/"
    cp "$mesa/../share/glvnd/egl_vendor.d/50_mesa.json" "$overlay/usr/share/glvnd/egl_vendor.d/"
    cp -a "$mesa/dri/libdril_dri.so" "$overlay/usr/lib/mesa/dri/"
    ln -sfn libdril_dri.so "$overlay/usr/lib/mesa/dri/zink_dri.so"
    ln -sfn libdril_dri.so "$overlay/usr/lib/mesa/dri/swrast_dri.so"
    cp "$hybris/libVkLayer_hybris_compat.so" "$hybris/VkLayer_hybris_compat.json" "$overlay/usr/lib/arlinux/vulkan/"
    api="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ICD"]["api_version"])' "$mesa/../share/vulkan/icd.d/freedreno_icd.aarch64.json")"
    python3 - "$overlay/usr/share/vulkan/icd.d/freedreno_icd.json" "$api" <<'PY'
import json,pathlib,sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({'file_format_version':'1.0.0','ICD':{'library_path':'../../../lib/mesa/libvulkan_freedreno.so','api_version':sys.argv[2]}},indent=2)+'\n')
PY
    cp "$overlay/usr/share/vulkan/icd.d/freedreno_icd.json" "$overlay/usr/share/vulkan/icd.d/arlinux_icd.json"
    # Container runtimes discover driver dependencies through the standard cache.
    mkdir -p "$overlay/etc/ld.so.conf.d"
    printf '/usr/lib/mesa\n' > "$overlay/etc/ld.so.conf.d/00-arlinux-graphics.conf"
    while IFS= read -r -d '' library; do
      readelf -h "$library" >/dev/null 2>&1 && set_relative_rpath "$overlay" "$library"
    done < <(find "$overlay/usr/lib/mesa" -type f -print0)
    tar -C "$overlay" --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf - . | zstd -T0 -19 -f -o "$assets/gpu-qualcomm.tar.zst"
    sha256sum "$assets/gpu-qualcomm.tar.zst" | cut -d' ' -f1 > "$assets/gpu-qualcomm-id"

    # Both profiles share Mesa/Zink and the application compatibility layer.
    # The generic profile uses Android's vendor Vulkan driver through libhybris.
    generic="$stage/$product/gpu-generic"
    mkdir -p "$generic"
    cp -a "$overlay/." "$generic/"
    rm -f "$generic/usr/lib/mesa/libvulkan_freedreno.so" \
      "$generic/usr/share/vulkan/icd.d/freedreno_icd.json"
    mkdir -p "$generic/usr/lib/hybris"
    cp -a "$hybris/." "$generic/usr/lib/hybris/"
    printf '/usr/lib/mesa\n/usr/lib/hybris\n' > "$generic/etc/ld.so.conf.d/00-arlinux-graphics.conf"
    python3 - "$generic/usr/share/vulkan/icd.d/hybris_icd.json" <<'PY'
import json,pathlib,sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({'file_format_version':'1.0.0','ICD':{'library_path':'../../../lib/hybris/libhybris-vulkan-icd.so.0','api_version':'1.3.0'}},indent=2)+'\n')
PY
    cp "$generic/usr/share/vulkan/icd.d/hybris_icd.json" "$generic/usr/share/vulkan/icd.d/arlinux_icd.json"
    while IFS= read -r -d '' library; do
      if readelf -h "$library" >/dev/null 2>&1; then
        set_relative_rpath "$generic" "$library"
      fi
    done < <(find "$generic/usr/lib/hybris" -type f -print0)
    tar -C "$generic" --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf - . | zstd -T0 -19 -f -o "$assets/gpu-generic.tar.zst"
    sha256sum "$assets/gpu-generic.tar.zst" | cut -d' ' -f1 > "$assets/gpu-generic-id"
    printf '%s\n' "${ids[$product]}" > "$assets/.inputs.sha256"
    echo "OK assets  $product"
done
