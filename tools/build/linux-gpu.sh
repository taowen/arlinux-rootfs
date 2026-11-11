#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
jobs="${ARLINUX_JOBS:-$(nproc)}"
component="${1:-all}"
case "$component" in all|mesa|hybris) ;; *) echo 'Expected all, mesa or hybris.' >&2; exit 2 ;; esac
cache_root="${ARLINUX_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/arlinux}"
mesa_install="$repo/build/linux/mesa"
hybris_stage="$repo/build/linux/libhybris"
mesa_revision="$(git -C "$repo/third_party/mesa" rev-parse HEAD)"
mesa_changes="$(git -C "$repo/third_party/mesa" diff --binary HEAD | sha256sum | cut -c1-16)"
mesa_cache="$cache_root/mesa-$mesa_revision-$mesa_changes"
mesa_source="$mesa_cache/source"
mesa_build="$mesa_cache/build"
gpu_inputs="$({
    uname -m
    aarch64-linux-gnu-gcc --version
    aarch64-linux-gnu-g++ --version
    meson --version
    aarch64-linux-gnu-pkg-config --modversion egl gl glesv2 libglvnd wayland-client x11
    sha256sum "$(command -v aarch64-linux-gnu-gcc)" \
        "$(aarch64-linux-gnu-gcc -print-prog-name=cc1)" \
        "$(aarch64-linux-gnu-g++ -print-prog-name=cc1plus)"
    printf '%s\n' "$mesa_revision"
    printf '%s\n' "$mesa_changes"
    git -C "$repo/third_party/libhybris" rev-parse HEAD
    git -C "$repo/third_party/libhybris" diff --binary HEAD | sha256sum
    (cd "$repo/third_party/libhybris" &&
        git ls-files --others --exclude-standard -z | sort -z | xargs -0 -r sha256sum)
    git -C "$repo/third_party/android-headers" rev-parse HEAD
    (cd "$repo" && {
        sha256sum tools/build/linux-gpu.sh tools/build/linux-aarch64.ini
        find third_party/libhybris/arlinux/graphics-protocols -type f -print0 | sort -z | xargs -0 sha256sum
    })
})"
gpu_id="$(printf '%s\n' "$gpu_inputs" | sha256sum | cut -d' ' -f1)"
gpu_id_file="$repo/build/linux/gpu.inputs.sha256"

gpu_receipt="$repo/build/linux/gpu.outputs.json"
verify_gpu_cache() {
    local receipt="${1:-$gpu_receipt}"
    if [[ $# -gt 0 ]]; then shift; else set -- "$mesa_install" "$hybris_stage/install"; fi
    python3 - "$repo" "$gpu_id" "$receipt" "$gpu_inputs" "$@" <<'CACHE'
import json,sys
from pathlib import Path
sys.path.insert(0,str(Path(sys.argv[1])/'tools/build'))
from pipeline_cache import inventory
try:
    record=json.loads(Path(sys.argv[3]).read_text())
    if record.get('format') != 1:
        raise ValueError('Unsupported GPU receipt format')
    if record.get('inputs') != sys.argv[2]:
        import difflib
        difference=difflib.unified_diff(record.get('inputDetails','').splitlines(),
            sys.argv[4].splitlines(),fromfile='producer',tofile='consumer',lineterm='')
        print('\n'.join(difference),file=sys.stderr)
        raise ValueError('GPU producer inputs differ: recorded='+str(record.get('inputs'))+
                         ' current='+sys.argv[2])
    actual={path:inventory(path) for path in sys.argv[5:]}
    if record.get('outputs') != actual:
        import itertools
        expected=record.get('outputs',{})
        def differences(before,after,path=''):
            if isinstance(before,dict) and isinstance(after,dict):
                for key in sorted(before.keys()|after.keys()):
                    yield from differences(before.get(key),after.get(key),path+'/'+key)
            elif before != after:
                yield path+': recorded='+repr(before)+' current='+repr(after)
        changes=list(itertools.islice(differences(expected,actual),8))
        raise ValueError('GPU output hashes differ:\n'+'\n'.join(changes))
except (OSError, ValueError) as error:
    print('GPU cache rejected: '+str(error),file=sys.stderr)
    raise SystemExit(1)
CACHE
}
if [[ "$component" == all && -f "$gpu_receipt" ]] && verify_gpu_cache; then
    printf '%s\n' "$mesa_install" "$hybris_stage/install/usr/lib/hybris"
    exit 0
fi

build_mesa() {
    if [[ ! -f "$mesa_source/meson.build" ]]; then
        rm -rf "$mesa_cache"
        mkdir -p "$mesa_source"
        git -C "$repo/third_party/mesa" archive "$mesa_revision" | tar -xf - -C "$mesa_source"
        git -C "$repo/third_party/mesa" diff --binary HEAD | git -C "$mesa_source" apply --allow-empty
    fi
    mkdir -p "$mesa_build" "$mesa_install"
    local setup=(meson setup "$mesa_build" "$mesa_source"
        --cross-file "$repo/tools/build/linux-aarch64.ini"
        -Dpkg_config_path="$repo/third_party/libhybris/arlinux/graphics-protocols"
        --prefix="$mesa_install" --libdir=lib --buildtype=release
        -Dauto_features=disabled -Dgallium-drivers=zink
        -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dfreedreno-force-sysmem=true
        -Dplatforms=x11,wayland -Darlinux-wsi=true
        -Degl-native-platform=auto -Degl=enabled -Dgles1=disabled
        -Dgles2=enabled -Dopengl=true -Dglx=dri -Dgbm=enabled -Dglvnd=enabled
        -Dgbm-backends-path=/usr/lib/mesa/gbm
        -Dllvm=disabled -Dzstd=disabled -Dshader-cache=false
        -Dxmlconfig=enabled -Dexpat=enabled -Dzlib=enabled
        -Dbuild-tests=false -Dtools= -Dvideo-codecs=)
    [[ -f "$mesa_build/build.ninja" ]] && setup+=(--reconfigure --clearcache)
    PKG_CONFIG_PATH="$repo/third_party/libhybris/arlinux/graphics-protocols" "${setup[@]}"
    ninja -C "$mesa_build" -j"$jobs"
    rm -rf -- "$mesa_install"
    ninja -C "$mesa_build" install
    cp -L /usr/lib/aarch64-linux-gnu/libvulkan.so.1 "$mesa_install/lib/libvulkan.so.1"
    for library in libGL.so.1 libEGL.so.1 libGLESv2.so.2 libGLX.so.0 libOpenGL.so.0 libGLdispatch.so.0; do
        cp -L "/usr/lib/aarch64-linux-gnu/$library" "$mesa_install/lib/$library"
    done
}

build_hybris() {
    local source="$hybris_stage/source" headers="$hybris_stage/headers"
    local install="$hybris_stage/install"
    rm -rf "$hybris_stage"
    mkdir -p "$source" "$headers" "$install"
    git -C "$repo/third_party/libhybris" archive HEAD | tar -xf - -C "$source"
    # Prevent discovery of the parent repository: otherwise apply silently
    # skips patch paths outside this staging directory's repository prefix.
    git -C "$repo/third_party/libhybris" diff --binary HEAD |
        GIT_CEILING_DIRECTORIES="$hybris_stage" git -C "$source" apply --allow-empty
    # Include new, non-ignored source files in a local development build.
    # git diff alone cannot carry files which have not been added to the index.
    (cd "$repo/third_party/libhybris" &&
        git ls-files --others --exclude-standard -z | tar --null -T - -cf -) |
        tar -xf - -C "$source"
    git -C "$repo/third_party/android-headers" archive HEAD | tar -xf - -C "$headers"
    pushd "$source/hybris" >/dev/null
    export PKG_CONFIG_PATH="$repo/third_party/libhybris/arlinux/graphics-protocols"
    export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
    export CC='ccache aarch64-linux-gnu-gcc' CXX='ccache aarch64-linux-gnu-g++'
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
    mkdir -p "$install/usr/lib/hybris/diagnostics"
    aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -pthread \
        "$source/arlinux/diagnostics/tls-probe.c" -ldl \
        -o "$install/usr/lib/hybris/diagnostics/tls-probe"
}

write_gpu_receipt() {
python3 - "$repo" "$gpu_id" "$1" "$gpu_inputs" "${@:2}" <<'RECEIPT'
import json,sys
from pathlib import Path
sys.path.insert(0,str(Path(sys.argv[1])/'tools/build'))
from pipeline_cache import inventory
receipt=Path(sys.argv[3]); pending=receipt.with_suffix('.json.part')
pending.write_text(json.dumps({'format':1,'inputs':sys.argv[2],
                              'inputDetails':sys.argv[4],
                              'outputs':{path:inventory(path) for path in sys.argv[5:]}},sort_keys=True)+'\n')
pending.replace(receipt)
RECEIPT
}
mesa_receipt="$repo/build/linux/gpu.mesa.outputs.json"
hybris_receipt="$repo/build/linux/gpu.hybris.outputs.json"
if [[ "$component" == all || "$component" == mesa ]]; then
    if ! verify_gpu_cache "$mesa_receipt" "$mesa_install"; then
        [[ "${ARLINUX_REQUIRE_GPU_CACHE:-0}" != 1 ]] || { echo 'Missing or invalid Mesa component artifact.' >&2; exit 2; }
        build_mesa
        write_gpu_receipt "$mesa_receipt" "$mesa_install"
    fi
fi
if [[ "$component" == all || "$component" == hybris ]]; then
    if ! verify_gpu_cache "$hybris_receipt" "$hybris_stage/install"; then
        [[ "${ARLINUX_REQUIRE_GPU_CACHE:-0}" != 1 ]] || { echo 'Missing or invalid libhybris component artifact.' >&2; exit 2; }
        build_hybris
        write_gpu_receipt "$hybris_receipt" "$hybris_stage/install"
    fi
fi
if [[ "$component" == all ]]; then
    printf '%s\n' "$gpu_id" > "$gpu_id_file"
    write_gpu_receipt "$gpu_receipt" "$mesa_install" "$hybris_stage/install"
fi
printf '%s\n' "$mesa_install" "$hybris_stage/install/usr/lib/hybris"
