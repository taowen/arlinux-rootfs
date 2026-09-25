#!/usr/bin/env bash
# Publish one verified bundle to its distribution repository's GitHub Releases.
set -euo pipefail

repo="${ARLINUX_ROOTFS_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
id="${1:?usage: publish-distribution.sh DISTRIBUTION TAG}"
tag="${2:?usage: publish-distribution.sh DISTRIBUTION TAG}"
[[ "$id" =~ ^[a-z][a-z0-9-]{0,63}$ ]] || { echo "Invalid distribution ID: $id" >&2; exit 2; }
[[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([.-][a-zA-Z0-9.-]+)?$ ]] || {
    echo "Use a version tag such as v0.1.0" >&2; exit 2;
}
product="$repo/distributions/$id"
[[ -f "$product/product.json" ]] || { echo "Unknown distribution: $id" >&2; exit 2; }
name="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$product/product.json")"
name="${name/#Arlinux/ARLinux}"
command -v gh >/dev/null || { echo 'Install and authenticate GitHub CLI (gh).' >&2; exit 2; }

# A release must describe committed, remotely available inputs, not local experiments.
for source in "$repo" "$product"; do
    [[ -z "$(git -C "$source" status --porcelain --untracked-files=normal)" ]] || {
        echo "Uncommitted changes in $source" >&2; exit 2;
    }
done
origin="$(git -C "$product" remote get-url origin)"
[[ "$origin" =~ github\.com[:/]([^/]+/[^/.]+)(\.git)?$ ]] || {
    echo "Distribution origin is not a GitHub repository: $origin" >&2; exit 2;
}
github_repo="${BASH_REMATCH[1]}"
commit="$(git -C "$product" rev-parse HEAD)"
[[ "$(git -C "$product" ls-remote origin HEAD | cut -f1)" == "$commit" ]] || {
    echo "Push the distribution commit to origin before publishing: $commit" >&2; exit 2;
}
if gh release view "$tag" -R "$github_repo" >/dev/null 2>&1; then
    echo "Release already exists: $github_repo $tag" >&2
    exit 2
fi

(cd "$repo" && ./build.sh build "$id" && ./build.sh verify "out/$id.zip")
bundle="$repo/out/$id.zip"
checksum="$(sha256sum "$bundle" | cut -d' ' -f1)"
size="$(stat -c %s "$bundle")"
protocol="$(python3 -c 'import json,sys,zipfile; print(json.loads(zipfile.ZipFile(sys.argv[1]).read("manifest.json"))["protocol"])' "$bundle")"
framework_commit="$(git -C "$repo" rev-parse HEAD)"
notes="$(printf 'Import this ZIP bundle into ARLinux 0.1.4 or later. The repository source archive is not the installable rootfs bundle. The bundle works with both the release and development Android package IDs.\n\n- Bundle protocol: %s\n- Distribution commit: `%s`\n- arlinux-rootfs commit: `%s`\n- Bundle size: %s bytes\n- SHA-256 (`%s.zip`): `%s`\n' "$protocol" "$commit" "$framework_commit" "$size" "$id" "$checksum")"
gh release create "$tag" "$bundle" -R "$github_repo" \
    --target "$commit" --title "$name $tag" --notes "$notes"
gh release view "$tag" -R "$github_repo" --json assets \
    --jq ".assets[] | select(.name == \"$id.zip\") | .url"
