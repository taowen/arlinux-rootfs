#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
bash build.sh
arlinux-app --apk app/build/outputs/apk/debug/app-debug.apk
