#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")" && pwd)"
cd "$repo"
command="${1:-build}"
shift || true
case "$command" in
  build)
    if [[ $# -eq 0 ]]; then set -- debian arch omarchy; fi
    git submodule update --init --recursive -- \
      third_party/android-headers third_party/libhybris third_party/mesa \
      "${@/#/distributions/}"
    ./tools/build/linux-assets.sh "$@"
    for product in "$@"; do
      python3 tools/bundle.py pack "distributions/$product" "out/$product.arlinux-rootfs"
    done
    ;;
  verify)
    for bundle in "$@"; do python3 tools/bundle.py verify "$bundle"; done
    ;;
  *) echo "usage: $0 build [debian|arch|omarchy ...] | verify BUNDLE..." >&2; exit 2 ;;
esac
