#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export ANDROID_HOME=${ARLINUX_ANDROID_SDK:-$HOME/.local/share/arlinux/android-sdk}
"$ANDROID_HOME/gradle-8.7/bin/gradle" "$@" assembleDebug
