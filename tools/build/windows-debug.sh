#!/usr/bin/env bash
# Build small diagnostics, not a new Wine runtime.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo 'Usage: windows-debug.sh LLVM_MINGW_BIN OUTPUT_DIR' >&2
    exit 2
fi
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
compiler="$(realpath "$1")/aarch64-w64-mingw32-clang"
output="$(realpath -m "$2")"
mkdir -p "$output"
"$compiler" -Wall -Wextra -Werror -municode \
    "$repo/third_party/proton/tests/arlinux/exception-observer.c" -ldbghelp -Wl,--no-insert-timestamp \
    -o "$output/exception-observer.exe"
"$compiler" -Wall -Wextra -Werror \
    "$repo/third_party/proton/tests/arlinux/exception-observer-probe.c" -Wl,--no-insert-timestamp \
    -o "$output/exception-observer-probe.exe"
sha256sum "$output/"*.exe
