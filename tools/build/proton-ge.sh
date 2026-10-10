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
[[ "$(git -C "$source_dir" describe --exact-match --tags HEAD)" == "$version" ]]
[[ "$(sha512sum "$base" | cut -d' ' -f1)" == "$expected" ]] || { echo 'Wrong GE archive SHA-512' >&2; exit 1; }
for tool in aarch64-linux-gnu-gcc aarch64-w64-mingw32-clang make autoconf bison flex patch; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 2; }
done
mkdir -p "$repo/build/proton-ge"
stage="$(mktemp -d "$repo/build/proton-ge/core.XXXXXXXX")"
echo "Build directory: $stage"
# Never run upstream reset/clean scripts in an existing developer checkout.
mkdir "$stage/wine"
tar -C "$source_dir/wine" --exclude=.git -cf - . | tar -C "$stage/wine" -xf -
cp "$repo/third_party/proton/patches/arlinux/0001-ntdll-map-executable-sections-before-relocations.patch" "$stage/android.patch"
if ! patch -d "$stage/wine" -p1 -R --dry-run < "$stage/android.patch" >/dev/null 2>&1; then
    patch -d "$stage/wine" -p1 --forward < "$stage/android.patch"
fi
# Generate tools from this same Wine source, not from a different Proton version.
mkdir "$stage/tools" "$stage/core"
(cd "$stage/tools" && "$stage/wine/configure" --enable-win64 --disable-tests \
    --without-mingw --without-x --without-wayland --without-freetype \
    && make -j"${JOBS:-8}" tools/widl/all tools/winebuild/all tools/wmc/all tools/wrc/all)
(cd "$stage/core" && "$stage/wine/configure" --host=aarch64-linux-gnu --enable-win64 \
    --with-wine-tools="$stage/tools" --disable-tests --without-x --without-wayland \
    --without-freetype --without-unwind \
    && make -j"${JOBS:-8}" dlls/ntdll/ntdll.so server/wineserver)
tar -xzf "$base" -C "$stage"
redist="$stage/$version-aarch64"
install -m755 "$stage/core/dlls/ntdll/ntdll.so" "$redist/files/lib/wine/aarch64-unix/ntdll.so"
install -m755 "$stage/core/server/wineserver" "$redist/files/bin-arm64/wineserver"
# Keep the exact modified source alongside the binary for reproducibility and licenses.
tar -czf "${output%.tar.gz}-wine-source.tar.gz" -C "$stage" wine android.patch
python3 "$repo/tools/package-proton.py" --source "$source_dir" --redist "$redist" \
    --integration-source "$repo/third_party/proton" \
    --wine-source-archive "${output%.tar.gz}-wine-source.tar.gz" --development --output "$output"
echo "Built $output (development bundle; Steam files were not modified)"
