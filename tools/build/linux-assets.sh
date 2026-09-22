#!/usr/bin/env bash
# Build the shared runtime and selected distribution bundles on Linux.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo"
export ARLINUX_CACHE_DIR="${ARLINUX_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/arlinux}"
host_package="${ARLINUX_HOST_PACKAGE:-io.taowen.arlinux}"
device_root="/data/user/0/$host_package/files/rootfs"
rebuild=false
if [[ "${1:-}" == --rebuild ]]; then rebuild=true; shift; fi
products=("$@")
if [[ ${#products[@]} -eq 0 ]]; then
    mapfile -t products < <(find distributions -mindepth 2 -maxdepth 2 \
        -name product.json -printf '%h\n' | sed 's|.*/||' | sort)
fi
gpu_inputs="$(git submodule status third_party/mesa third_party/libhybris third_party/android-headers)"

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
    local inputs=(native/bionicx-runtime runtime protocols examples tools/build tools/arlinux-app-data)
    {
        git ls-files -s -- "${inputs[@]}"
        git diff --binary -- "${inputs[@]}"
        git ls-files --others --exclude-standard -z -- \
            "${inputs[@]}" \
            | sort -z | xargs -0 -r sha256sum
        git -C "distributions/$product" ls-files -s guest native tools \
            product.json profile.json rootfs.lock.json
        git -C "distributions/$product" diff --binary -- guest native tools \
            product.json profile.json rootfs.lock.json
        (cd "distributions/$product" &&
            git ls-files --others --exclude-standard -z -- \
                guest native tools product.json profile.json rootfs.lock.json \
                | sort -z | xargs -0 -r sha256sum)
        git -C "distributions/$product" submodule status --recursive || true
        printf '%s\n' "$gpu_inputs"
        printf '%s\n' "$host_package"
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
runtime_sources=(
  native/bionicx-runtime/android-kernel.c native/bionicx-runtime/dns.c
  native/bionicx-runtime/fhs-path.c native/bionicx-runtime/fhs-env.c
  native/bionicx-runtime/fhs-exec.c native/bionicx-runtime/fhs-fork.c
  native/bionicx-runtime/fhs-pty.c native/bionicx-runtime/fhs-metadata.c
  native/bionicx-runtime/identity.c native/bionicx-runtime/namespace.c
  native/bionicx-runtime/waitid-emu.c)
for product in "${pending[@]}"; do
    output="build/linux/runtime/$product"; mkdir -p "$output"
    aarch64-linux-gnu-gcc -shared -fPIC -O2 -Wall -Wextra -Werror \
      -Wno-error=unused-result -Wno-error=nonnull-compare \
      -DBIONICX_GLIBC_INTERPOSE -I"distributions/$product/native" \
      "${runtime_sources[@]}" -o "$output/libbionicx-runtime.so" \
      -Wl,-z,now -ldl -pthread -Wl,--version-script=native/bionicx-runtime/glibc-interpose.map
    aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror native/bionicx-runtime/sudo.c -o "$output/sudo"
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
declare -A glibc_outputs
for product in "${pending[@]}"; do
    version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["glibcVersion"])' "distributions/$product/product.json")"
    dirs="$(python3 -c 'import json,sys; print(":".join(json.load(open(sys.argv[1]))["libraryDirectories"]))' "distributions/$product/product.json")"
    glibc_outputs[$product]="$(BIONICX_GLIBC_VERSION="$version" \
      BIONICX_GLIBC_PREFIX="$device_root" \
      BIONICX_GLIBC_LIBRARY_DIRS="$dirs" "$repo/tools/build/linux-glibc.sh" | tail -n 1)"
done

echo '== Product root filesystems =='
stage="${ARLINUX_PRODUCT_STAGE:-$ARLINUX_CACHE_DIR/products}"; mkdir -p "$stage"
for product in "${pending[@]}"; do
    product_dir="$repo/distributions/$product"; rootfs="$stage/$product/rootfs"
    assets="$product_dir/build/assets"; glibc="${glibc_outputs[$product]}"
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
    mkdir -p "$rootfs/usr/lib/arlinux/guest" "$rootfs/usr/lib/arlinux-platform"
    cp tools/arlinux-app-data/hosted-ime.py tools/arlinux-app-data/org.arlinux.HostedInput.service \
      "$rootfs/usr/lib/arlinux/"
    install -Dm644 tools/arlinux-app-data/org.arlinux.HostedInput.service \
      "$rootfs/usr/share/dbus-1/services/org.arlinux.HostedInput.service"
    cp -a "$product_dir/guest/." "$rootfs/usr/lib/arlinux/guest/"
    cp examples/desk-auto/dump-atspi.py examples/desk-auto/atspi-do.py \
      "$rootfs/usr/lib/arlinux/guest/"
    chmod 755 "$rootfs/usr/lib/arlinux/guest/"*.py \
      "$rootfs/usr/lib/arlinux/guest/"*.sh
    [[ ! -f "$rootfs/usr/lib/arlinux/guest/arlinux-a11y" ]] ||
      chmod 755 "$rootfs/usr/lib/arlinux/guest/arlinux-a11y"
    cp "$glibc/ld-linux-aarch64.so.1" "$glibc/libc.so.6" "$glibc/libm.so.6" "$glibc/ldconfig" \
      "$rootfs/usr/lib/arlinux-platform/"
    python3 tools/relocate-shebangs.py "$rootfs" --device-root "$device_root"
    python3 - "$rootfs" <<'PY'
import os, pathlib, sys
root = pathlib.Path(sys.argv[1])
for directory, dirs, files in os.walk(root, followlinks=False):
    for name in dirs + files:
        path = pathlib.Path(directory) / name
        if path.is_symlink():
            target = os.readlink(path)
            if target.startswith('/') and target.split('/')[1] not in ('proc','sys','dev'):
                path.unlink(); path.symlink_to(os.path.relpath(root / target.lstrip('/'), path.parent))
for name, contents in [('etc/resolv.conf','nameserver 1.1.1.1\n'), ('etc/machine-id','')]:
    path = root / name
    if path.is_symlink(): path.unlink()
    path.parent.mkdir(parents=True, exist_ok=True); path.write_text(contents)
# An unprivileged guest observes its process mount table through procfs.
mtab = root / 'etc/mtab'
if not os.path.lexists(mtab):
    mtab.symlink_to('/proc/self/mounts')
PY
    mkdir -p "$assets/bionicx/lib" "$assets/arlinux"
    cp -a "$product_dir/guest" "$assets/guest"
    cp examples/desk-auto/dump-atspi.py examples/desk-auto/atspi-do.py "$assets/guest/"
    chmod 755 "$assets/guest/"*.py "$assets/guest/"*.sh
    [[ ! -f "$assets/guest/arlinux-a11y" ]] ||
      chmod 755 "$assets/guest/arlinux-a11y"
    cp "$glibc/ld-linux-aarch64.so.1" "$glibc/libc.so.6" "$glibc/libm.so.6" "$glibc/ldconfig" "$assets/bionicx/lib/"
    cp "build/linux/runtime/$product/libbionicx-runtime.so" "$assets/bionicx/lib/"
    cp "build/linux/runtime/$product/sudo" "$assets/bionicx/sudo"
    cp "$teapot/teapot-glx" "$teapot/teapot-egl" "$assets/arlinux/"
    cp tools/arlinux-app-data/accessibility-session.sh "$assets/arlinux/"
    cp -a tools/arlinux-app-data/fonts "$assets/arlinux/fonts"
    python3 - "$product_dir" "$assets" "$repo/profiles/xterm.json" <<'PY'
import json, pathlib, sys
product, assets, fallback = map(pathlib.Path, sys.argv[1:])
config = json.loads((product/'product.json').read_text())
profile_file = product/'profile.json'; profile_file = profile_file if profile_file.is_file() else fallback
profile = json.loads(profile_file.read_text()); profile['launch']['environment'].update(config.get('environment',{}))
(assets/'xterm.json').write_text(json.dumps(profile,indent=2)+'\n')
(assets/'guest.properties').write_text('distributionId='+product.name+'\nrequiredFiles='+','.join(config['requiredFiles'])+'\nlibraryDirectories='+','.join(config['libraryDirectories'])+'\n')
PY
    tar -C "$rootfs" --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
      --exclude=./proc --exclude=./sys --exclude=./dev --exclude=./run --exclude=./tmp \
      --exclude=./var/run --exclude=./var/lock -cf - . | zstd -T0 -8 -f -o "$assets/rootfs.tar.zst"
    sha256sum "$assets/rootfs.tar.zst" | cut -d' ' -f1 > "$assets/rootfs-seed-id"

    overlay="$stage/$product/gpu"; deploy="$device_root"
    rpath="$deploy/usr/lib/mesa:$deploy/usr/lib/arlinux-platform:$deploy/usr/lib:$deploy/lib"
    mkdir -p "$overlay/usr/lib/mesa/dri" \
      "$overlay/usr/lib/arlinux/vulkan" "$overlay/usr/share/vulkan/icd.d"
    cp -a "$mesa"/libEGL.so* "$mesa"/libGLESv2.so* "$mesa"/libGL.so* "$mesa"/libgallium-*.so \
      "$mesa/libvulkan_freedreno.so" "$mesa/libvulkan.so.1" "$overlay/usr/lib/mesa/"
    cp -a "$mesa/dri/libdril_dri.so" "$overlay/usr/lib/mesa/dri/"
    ln -sfn libdril_dri.so "$overlay/usr/lib/mesa/dri/zink_dri.so"
    ln -sfn libdril_dri.so "$overlay/usr/lib/mesa/dri/swrast_dri.so"
    for link in libGLX.so.0 libGLX.so.1 libGLX.so.0.0.0 libGLX.so libGLX_mesa.so.0 libOpenGL.so.0 libOpenGL.so; do
      ln -sfn libGL.so.1.2.0 "$overlay/usr/lib/mesa/$link"
    done
    cp "$hybris/libVkLayer_hybris_compat.so" "$hybris/VkLayer_hybris_compat.json" "$overlay/usr/lib/arlinux/vulkan/"
    api="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ICD"]["api_version"])' "$mesa/../share/vulkan/icd.d/freedreno_icd.aarch64.json")"
    python3 - "$overlay/usr/share/vulkan/icd.d/freedreno_icd.json" "$api" <<'PY'
import json,pathlib,sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({'file_format_version':'1.0.0','ICD':{'library_path':'../../../lib/mesa/libvulkan_freedreno.so','api_version':sys.argv[2]}},indent=2)+'\n')
PY
    cp "$overlay/usr/share/vulkan/icd.d/freedreno_icd.json" "$overlay/usr/share/vulkan/icd.d/arlinux_icd.json"
    while IFS= read -r -d '' library; do readelf -h "$library" >/dev/null 2>&1 && patchelf --set-rpath "$rpath" "$library"; done < <(find "$overlay/usr/lib/mesa" -type f -print0)
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
    python3 - "$generic/usr/share/vulkan/icd.d/hybris_icd.json" <<'PY'
import json,pathlib,sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({'file_format_version':'1.0.0','ICD':{'library_path':'../../../lib/hybris/libhybris-vulkan-icd.so.0','api_version':'1.3.0'}},indent=2)+'\n')
PY
    cp "$generic/usr/share/vulkan/icd.d/hybris_icd.json" "$generic/usr/share/vulkan/icd.d/arlinux_icd.json"
    while IFS= read -r -d '' library; do
      if readelf -h "$library" >/dev/null 2>&1; then
        patchelf --set-rpath "$rpath:$deploy/usr/lib/hybris:$deploy/usr/lib/hybris/libhybris" "$library"
      fi
    done < <(find "$generic/usr/lib/hybris" -type f -print0)
    tar -C "$generic" --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf - . | zstd -T0 -19 -f -o "$assets/gpu-generic.tar.zst"
    sha256sum "$assets/gpu-generic.tar.zst" | cut -d' ' -f1 > "$assets/gpu-generic-id"
    printf '%s\n' "${ids[$product]}" > "$assets/.inputs.sha256"
    echo "OK assets  $product"
done
