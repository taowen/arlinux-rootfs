#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")" && pwd)"
cd "$repo"
command="${1:-build}"
shift || true

products() {
  find distributions -mindepth 2 -maxdepth 2 -name product.json -printf '%h\n' \
    | sed 's|.*/||' | sort
}

select_products() {
  if [[ $# -eq 0 ]]; then mapfile -t selected < <(products); else selected=("$@"); fi
  [[ ${#selected[@]} -gt 0 ]] || { echo 'No distributions found.' >&2; exit 2; }
  for product in "${selected[@]}"; do
    [[ "$product" =~ ^[a-z][a-z0-9-]{0,63}$ ]] || {
      echo "Invalid distribution ID: $product" >&2; exit 2;
    }
    [[ -f "distributions/$product/product.json" ]] || {
      echo "Unknown distribution: $product" >&2; exit 2;
    }
    python3 tools/validate-distribution.py "distributions/$product"
  done
}

case "$command" in
  build)
    select_products "$@"
    git submodule update --init --recursive
    ./tools/build/linux-assets.sh "${selected[@]}"
    for product in "${selected[@]}"; do
      python3 tools/bundle.py pack "distributions/$product" "out/$product.arlinux-rootfs"
    done
    ;;
  doctor)
    ./tools/build/doctor.sh
    ;;
  list)
    products
    ;;
  validate)
    [[ $# -gt 0 ]] || { echo 'validate requires a distribution directory' >&2; exit 2; }
    for distribution in "$@"; do python3 tools/validate-distribution.py "$distribution"; done
    ;;
  verify)
    [[ $# -gt 0 ]] || { echo 'verify requires at least one bundle' >&2; exit 2; }
    for bundle in "$@"; do python3 tools/bundle.py verify "$bundle"; done
    ;;
  *) echo "usage: $0 {build [DISTRIBUTION...]|doctor|list|validate DIRECTORY...|verify BUNDLE...}" >&2; exit 2 ;;
esac
