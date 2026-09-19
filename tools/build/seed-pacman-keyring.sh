#!/usr/bin/env bash
set -euo pipefail
root=${1:?rootfs required}
shift
home=$root/etc/pacman.d/gnupg
mkdir -p "$home"
chmod 700 "$home"
keyrings=$root/usr/share/pacman/keyrings
keys=() trusted=()
for name in "$@"; do
    keys+=("$keyrings/$name.gpg")
    trusted+=("$keyrings/$name-trusted")
done
# GnuPG's root-owned trust database cannot be created on Android, where the
# extracted tree is owned by the app UID. Generate it with the signed seed.
gpg --homedir "$home" --no-default-keyring --keyring "$home/pubring.gpg" \
    --batch --quiet --import "${keys[@]}"
awk -F: 'NF >= 2 && $1 ~ /^[[:xdigit:]]{40}$/ { print $1 ":6:" }' \
    "${trusted[@]}" |
    gpg --homedir "$home" --batch --quiet --import-ownertrust
gpg --homedir "$home" --batch --quiet --check-trustdb
touch "$home/secring.gpg" "$home/arlinux-populated"
chmod 644 "$home/pubring.gpg" "$home/trustdb.gpg"
chmod 600 "$home/secring.gpg"
