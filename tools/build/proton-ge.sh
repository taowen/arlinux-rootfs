#!/usr/bin/env bash
# Rebuild the Android-sensitive Wine core against GE's matching prepared source.
# The rest of GE-Proton, including its FEX integration, is kept intact.
set -euo pipefail
if [[ $# != 4 ]]; then
    echo 'Usage: proton-ge.sh GE_SOURCE GE_ARCHIVE LLVM_MINGW_BIN OUTPUT.tar.gz' >&2
    echo 'GE_SOURCE must have completed GE upstream Wine preparation.' >&2
    exit 2
fi
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source_dir="$(realpath "$1")"
base="$(realpath "$2")"
export PATH="$(realpath "$3"):$PATH"
output="$(realpath -m "$4")"
version=GE-Proton11-7
expected=741cf70256f13b20d44952b590defd68b115814097f911c9ec053a64d33795267e2a982a5ce939407e8b649613eb3943bb2e2ebf4794d302ff96b83b61457cdd
source_revision="$(git -C "$source_dir" rev-parse HEAD)"
fork_revision="$(git -C "$repo/third_party/proton" rev-parse HEAD)"
[[ "$source_revision" == c191f35dcebbeccfacd3b4c6f6eea026e588c1c2 || "$source_revision" == "$fork_revision" ]] \
    || { echo 'Use the pinned GE upstream revision or the pinned ARLinux fork.' >&2; exit 2; }
[[ "$(git -C "$source_dir/wine" rev-parse HEAD)" == 46b29104e3741fe23bf5e2547196a253aab88c89 ]] \
    || { echo 'Wine source does not match the GE ARM64 archive.' >&2; exit 2; }
[[ "$(sha512sum "$base" | cut -d' ' -f1)" == "$expected" ]] || { echo 'Wrong GE archive SHA-512' >&2; exit 1; }
for tool in aarch64-linux-gnu-gcc aarch64-w64-mingw32-clang make autoconf bison flex patch; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 2; }
done
aarch64-w64-mingw32-clang -marm64x -fsyntax-only -x c /dev/null \
    || { echo 'LLVM-MinGW with ARM64X support (LLVM 23+) is required.' >&2; exit 2; }
mkdir -p "$repo/build/proton-ge"
stage="$(mktemp -d "$repo/build/proton-ge/core.XXXXXXXX")"
echo "Build directory: $stage"
# Never run upstream reset/clean scripts in an existing developer checkout.
mkdir "$stage/wine"
tar -C "$source_dir/wine" --exclude=.git -cf - . | tar -C "$stage/wine" -xf -
cp -a "$repo/third_party/proton/patches/arlinux" "$stage/android-patches"
for android_patch in "$stage/android-patches"/*.patch; do
    if ! patch -d "$stage/wine" -p1 -R --dry-run < "$android_patch" >/dev/null 2>&1; then
        patch -d "$stage/wine" -p1 --forward < "$android_patch"
    fi
done
# Generate tools from this same Wine source, not from a different Proton version.
mkdir "$stage/tools" "$stage/core"
(cd "$stage/tools" && "$stage/wine/configure" --enable-win64 --disable-tests \
    --without-mingw --without-x --without-wayland --without-freetype \
    && make -j"${JOBS:-8}" tools/widl/all tools/winebuild/all tools/winegcc/all tools/wmc/all tools/wrc/all)
# Keep GE's ARM64EC/ARM64X builtins usable by both native ARM64 and x64 clients.
(cd "$stage/core" && "$stage/wine/configure" --host=aarch64-linux-gnu --enable-win64 \
    --enable-archs=arm64ec,aarch64,i386,x86_64 \
    --with-wine-tools="$stage/tools" --disable-tests --without-x --without-wayland \
    --without-freetype --without-unwind \
    && make -j"${JOBS:-8}" dlls/ntdll/ntdll.so dlls/nsiproxy.sys/nsiproxy.so server/wineserver \
        dlls/iphlpapi/aarch64-windows/iphlpapi.dll \
        dlls/iphlpapi/i386-windows/iphlpapi.dll dlls/iphlpapi/x86_64-windows/iphlpapi.dll)
tar -xzf "$base" -C "$stage"
redist="$stage/$version-aarch64"
install -m755 "$stage/core/dlls/ntdll/ntdll.so" "$redist/files/lib/wine/aarch64-unix/ntdll.so"
install -m755 "$stage/core/dlls/nsiproxy.sys/nsiproxy.so" "$redist/files/lib/wine/aarch64-unix/nsiproxy.so"
install -m755 "$stage/core/server/wineserver" "$redist/files/bin-arm64/wineserver"
for arch in aarch64 i386 x86_64; do
    install -m755 "$stage/core/dlls/iphlpapi/$arch-windows/iphlpapi.dll" \
        "$redist/files/lib/wine/$arch-windows/iphlpapi.dll"
done
# GE's DXVK 3.x requires storageBuffer8BitAccess, unavailable on Adreno 6xx.
# Keep the Windows-app runtime GPU accelerated with one Vulkan 1.3 baseline.
bash "$repo/tools/build/dxvk-windows.sh" "$3" "$stage/dxvk"
for arch in aarch64 i386 x86_64; do
    cp "$stage/dxvk/$arch-windows/"*.dll "$redist/files/lib/wine/dxvk/$arch-windows/"
done
mkdir -p "$redist/files/share/arlinux-dxvk"
cp "$stage/dxvk/source-commit" "$stage/dxvk/source.tar.gz" \
    "$stage/dxvk/"*.patch "$redist/files/share/arlinux-dxvk/"
# Keep the exact modified source alongside the binary for reproducibility and licenses.
tar -czf "${output%.tar.gz}-wine-source.tar.gz" -C "$stage" wine android-patches
python3 "$repo/tools/package-proton.py" --source "$source_dir" --redist "$redist" \
    --integration-source "$repo/third_party/proton" \
    --wine-source-archive "${output%.tar.gz}-wine-source.tar.gz" --development --output "$output"
echo "Built $output (development bundle; Steam files were not modified)"
