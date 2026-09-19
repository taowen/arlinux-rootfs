#!/usr/bin/env python3
"""Relocate fixed-width Android-private prefixes embedded in an app tree."""

import argparse
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("root", type=Path)
parser.add_argument("--from-prefix", required=True)
parser.add_argument("--to-prefix", required=True)
args = parser.parse_args()

old = args.from_prefix.encode()
new = args.to_prefix.encode()
if len(old) != len(new):
    parser.error("prefixes must have identical byte lengths; use patchelf for PT_INTERP otherwise")
if not args.root.is_dir():
    parser.error(f"not a directory: {args.root}")

changed = []
for path in sorted(item for item in args.root.rglob("*") if item.is_file()):
    try:
        data = path.read_bytes()
    except OSError:
        continue
    if old not in data:
        continue
    path.write_bytes(data.replace(old, new))
    changed.append(path)

for path in changed:
    print(path)
print(f"relocated {len(changed)} files")
