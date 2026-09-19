#!/usr/bin/env python3
"""Relocate executable FHS shebang interpreters into an Android app rootfs."""

import argparse
import os
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("rootfs", type=Path)
parser.add_argument("--device-root", required=True)
args = parser.parse_args()

rootfs = args.rootfs.resolve()
device_root = args.device_root.rstrip("/")
relocated = 0

for path in rootfs.rglob("*"):
    if path.is_symlink() or not path.is_file():
        continue
    if not path.stat().st_mode & 0o111:
        continue
    with path.open("rb") as stream:
        first = stream.readline(4096)
        rest = stream.read()
    if not first.startswith(b"#!/"):
        continue
    line = first.rstrip(b"\r\n")
    interpreter = line[2:].split(None, 1)[0].decode("utf-8", "strict")
    rooted_interpreter = rootfs / interpreter.removeprefix("/")
    if not rooted_interpreter.exists():
        continue
    replacement = b"#!" + device_root.encode() + line[2:] + b"\n"
    if len(replacement) > 255:
        raise SystemExit(f"relocated shebang exceeds Linux limit: {path}")
    with path.open("r+b") as stream:
        stream.write(replacement)
        stream.write(rest)
        stream.truncate()
    relocated += 1

print(f"relocated {relocated} executable script shebangs")
