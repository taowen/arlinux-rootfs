#!/usr/bin/env python3
"""Reject ELF objects that require a newer numeric GLIBC symbol version."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path


GLIBC_VERSION = re.compile(r"\bGLIBC_(\d+)\.(\d+)\b")

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("root", type=Path)
parser.add_argument("--maximum", default="2.41")
parser.add_argument("--readelf", default="readelf")
args = parser.parse_args()

maximum = tuple(int(part) for part in args.maximum.split("."))
if len(maximum) != 2:
    parser.error("--maximum must have the form MAJOR.MINOR")

checked = 0
violations: list[tuple[Path, tuple[int, int]]] = []
for path in sorted(args.root.rglob("*")):
    if not path.is_file() or path.is_symlink():
        continue
    with path.open("rb") as source:
        if source.read(4) != b"\x7fELF":
            continue
    result = subprocess.run(
        [args.readelf, "--version-info", str(path)],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(f"readelf failed for {path}: {result.stderr.strip()}")
    checked += 1
    required = {tuple(map(int, match)) for match in GLIBC_VERSION.findall(result.stdout)}
    newer = sorted(version for version in required if version > maximum)
    if newer:
        violations.append((path, newer[-1]))

for path, version in violations:
    print(f"{path}: requires GLIBC_{version[0]}.{version[1]}")
if violations:
    raise SystemExit(1)
print(f"checked {checked} ELF objects; maximum required GLIBC version <= {args.maximum}")
