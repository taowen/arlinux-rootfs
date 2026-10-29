#!/usr/bin/env bash
# Single Linux product dispatcher. See docs/BUILD.md.
set -euo pipefail
repo="$(cd "$(dirname "$0")" && pwd)"
cd "$repo"
command="${1:---help}"
shift || true
case "$command" in
  guest-tools) exec python3 tools/build/guest-tools.py "$@" ;;
  capture) exec python3 tools/build/vulkan-capture-component.py "$@" ;;
  input) exec python3 tools/build/ibus-component.py "$@" ;;
  gpu) exec python3 tools/build/linux-gpu-component.py "$@" ;;
  github) exec python3 tools/build/github-products.py "$@" ;;
  rootfs) exec python3 tools/build/desktop-pipeline.py "$@" ;;
  publish-rootfs) exec python3 tools/publish-distribution.py "$@" ;;
  doctor) exec bash tools/build/doctor.sh "$@" ;;
  list) find distributions -mindepth 2 -maxdepth 2 -name product.json -printf '%h\n' | sed 's|.*/||' | sort ;;
  validate) [[ $# -gt 0 ]] || exit 2; for p in "$@"; do python3 tools/validate-distribution.py "$p"; done ;;
  verify) [[ $# -gt 0 ]] || exit 2; for p in "$@"; do python3 tools/bundle.py verify "$p"; done ;;
  --help|-h) echo 'Usage: ./build.sh {rootfs|publish-rootfs|doctor|list|validate|verify} ...'; echo 'Canonical guide: docs/BUILD.md' ;;
  *) echo "Unsupported command: $command. See docs/BUILD.md; build/prepare/offline are removed." >&2; exit 2 ;;
esac
