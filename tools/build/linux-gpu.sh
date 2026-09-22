#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
jobs="${ARLINUX_JOBS:-$(nproc)}"
cache_root="${ARLINUX_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/arlinux}"
mesa_install="$repo/build/linux/mesa"
hybris_stage="$repo/build/linux/libhybris"
mesa_revision="$(git -C "$repo/third_party/mesa" rev-parse HEAD)"
mesa_cache="$cache_root/mesa-$mesa_revision"
mesa_source="$mesa_cache/source"
mesa_build="$mesa_cache/build"
gpu_id="$({
    printf '%s\n' "$mesa_revision"
    git -C "$repo/third_party/libhybris" rev-parse HEAD
    git -C "$repo/third_party/android-headers" rev-parse HEAD
    sha256sum "$repo/tools/build/linux-gpu.sh" "$repo/tools/build/linux-aarch64.ini"
    find "$repo/graphics-protocols" -type f -print0 | sort -z | xargs -0 sha256sum
} | sha256sum | cut -d' ' -f1)"
gpu_id_file="$repo/build/linux/gpu.inputs.sha256"

if [[ -f "$mesa_install/lib/libvulkan_freedreno.so" \
      && -f "$hybris_stage/install/usr/lib/hybris/libVkLayer_hybris_compat.so" ]]; then
    if [[ ! -f "$gpu_id_file" || "$(cat "$gpu_id_file")" == "$gpu_id" ]]; then
        mkdir -p "$(dirname "$gpu_id_file")"
        printf '%s\n' "$gpu_id" > "$gpu_id_file"
        printf '%s\n' "$mesa_install" "$hybris_stage/install/usr/lib/hybris"
        exit 0
    fi
fi

build_mesa() {
    if [[ ! -f "$mesa_source/meson.build" ]]; then
        rm -rf "$mesa_cache"
        mkdir -p "$mesa_source"
        git -C "$repo/third_party/mesa" archive "$mesa_revision" | tar -xf - -C "$mesa_source"
    fi
    mkdir -p "$mesa_build" "$mesa_install"
    local setup=(meson setup "$mesa_build" "$mesa_source"
        --cross-file "$repo/tools/build/linux-aarch64.ini"
        -Dpkg_config_path="$repo/graphics-protocols"
        --prefix="$mesa_install" --libdir=lib --buildtype=release
        -Dauto_features=disabled -Dgallium-drivers=zink
        -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl
        -Dplatforms=x11,wayland -Darlinux-wsi=true
        -Degl-native-platform=auto -Degl=enabled -Dgles1=disabled
        -Dgles2=enabled -Dopengl=true -Dglx=dri -Dgbm=disabled
        -Dllvm=disabled -Dzstd=disabled -Dshader-cache=false
        -Dxmlconfig=enabled -Dexpat=enabled -Dzlib=enabled
        -Dbuild-tests=false -Dtools= -Dvideo-codecs=)
    [[ -f "$mesa_build/build.ninja" ]] && setup+=(--reconfigure --clearcache)
    PKG_CONFIG_PATH="$repo/graphics-protocols" "${setup[@]}"
    ninja -C "$mesa_build" -j"$jobs" install
    cp -L /usr/lib/aarch64-linux-gnu/libvulkan.so.1 "$mesa_install/lib/libvulkan.so.1"
}

build_hybris() {
    local source="$hybris_stage/source" headers="$hybris_stage/headers"
    local install="$hybris_stage/install"
    rm -rf "$hybris_stage"
    mkdir -p "$source" "$headers" "$install"
    git -C "$repo/third_party/libhybris" archive HEAD | tar -xf - -C "$source"
    git -C "$repo/third_party/android-headers" archive HEAD | tar -xf - -C "$headers"
    pushd "$source/hybris" >/dev/null
    export PKG_CONFIG_PATH="$repo/graphics-protocols"
    export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
    export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
    export AR=aarch64-linux-gnu-ar STRIP=aarch64-linux-gnu-strip
    export RANLIB=aarch64-linux-gnu-ranlib LD=aarch64-linux-gnu-ld
    export NM=aarch64-linux-gnu-nm OBJDUMP=aarch64-linux-gnu-objdump
    export CPPFLAGS="-I$repo/third_party/mesa/include -idirafter /usr/include"
    export LDFLAGS='-Wl,--no-as-needed'
    NOCONFIGURE=1 ./autogen.sh
    ./configure --host=aarch64-linux-gnu --prefix=/usr/lib/hybris \
        --libdir=/usr/lib/hybris --with-android-headers="$headers" \
        --enable-arch=arm64 --enable-wayland --enable-x11 \
        --disable-wayland_serverside_buffers --enable-adreno-quirks \
        --enable-mali-quirks --enable-property-cache \
        --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64:/system_ext/lib64
    make -C common -j"$jobs" SUBDIRS=. libhybris-common.la
    make -C common install-libLTLIBRARIES DESTDIR="$install"
    make -C common/q -j"$jobs"
    make -C common/q install DESTDIR="$install"
    for dir in include properties libsync platforms hardware ui gralloc egl glesv1 glesv2 hwc2 vulkan utils; do
        [[ -f "$dir/Makefile" ]] || continue
        make -C "$dir" -j"$jobs"
        make -C "$dir" install DESTDIR="$install"
    done
    popd >/dev/null
}

build_mesa
build_hybris
printf '%s\n' "$gpu_id" > "$gpu_id_file"
printf '%s\n' "$mesa_install" "$hybris_stage/install/usr/lib/hybris"
