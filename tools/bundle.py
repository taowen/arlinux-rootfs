#!/usr/bin/env python3
"""Pack or verify a self-contained Arlinux distribution bundle (protocol v3)."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile


PROTOCOL = 3
PAYLOADS = ("rootfs.tar.zst", "rootfs-seed-id", "gpu-qualcomm.tar.zst", "gpu-qualcomm-id",
            "gpu-generic.tar.zst", "gpu-generic-id", "guest.properties", "bionicx/lib/ld-linux-aarch64.so.1",
            "bionicx/lib/libc.so.6", "bionicx/lib/libm.so.6",
            "bionicx/lib/ldconfig",
            "bionicx/sudo", "profile.json")
IDENTIFIER = re.compile(r"[a-z][a-z0-9-]{0,63}\Z")


def digest(stream) -> str:
    result = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        result.update(chunk)
    return result.hexdigest()


def check_manifest(manifest: dict) -> None:
    if manifest.get("protocol") != PROTOCOL:
        raise ValueError("unsupported rootfs protocol")
    if not IDENTIFIER.fullmatch(manifest.get("distributionId", "")):
        raise ValueError("invalid distribution ID")
    if "hostPackage" in manifest:
        raise ValueError("bundle must not name an Android package")
    if manifest.get("compositor") not in ("anlabwc", "hyprland"):
        raise ValueError("invalid compositor hint")
    for key in ("requiredFiles", "libraryDirectories"):
        values = manifest.get(key)
        if not isinstance(values, list) or not values or any(
            not isinstance(path, str) or path.startswith("/") or
            any(part in ("", ".", "..") for part in path.split("/")) for path in values
        ):
            raise ValueError("invalid " + key)
    files = manifest.get("files")
    if not isinstance(files, dict) or not set(PAYLOADS).issubset(files):
        raise ValueError("missing payloads")
    if any(not re.fullmatch(r"[0-9a-f]{64}", value) for value in files.values()):
        raise ValueError("invalid payload checksum")


def pack(product: Path, target: Path) -> None:
    config = json.loads((product / "product.json").read_text(encoding="utf-8"))
    assets = product / "build/assets"
    files = {}
    for path in sorted(assets.rglob("*")):
        if not path.is_file() or path.name == ".inputs.sha256":
            continue
        name = path.relative_to(assets).as_posix()
        with path.open("rb") as source:
            files[name] = digest(source)
    manifest = {
        "protocol": PROTOCOL,
        "distributionId": product.name,
        "name": config["name"],
        "compositor": config.get("compositor", "anlabwc"),
        "requiredFiles": config["requiredFiles"],
        "libraryDirectories": config["libraryDirectories"],
        "files": files,
    }
    check_manifest(manifest)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(target.suffix + ".part")
    with zipfile.ZipFile(temporary, "w", allowZip64=True) as archive:
        archive.writestr("manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        for name in files:
            archive.write(assets / name, name, compress_type=zipfile.ZIP_STORED)
    temporary.replace(target)
    print(target)


def verify(path: Path) -> None:
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("duplicate zip entry")
        manifest = json.loads(archive.read("manifest.json"))
        check_manifest(manifest)
        if set(names) != {"manifest.json", *manifest["files"]}:
            raise ValueError("unexpected bundle entries")
        for name, expected in manifest["files"].items():
            if name.startswith("/") or ".." in name.split("/"):
                raise ValueError("unsafe entry name")
            with archive.open(name) as source:
                if digest(source) != expected:
                    raise ValueError("checksum mismatch: " + name)
    print(f"OK {manifest['distributionId']} protocol {PROTOCOL}: {path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack_cmd = commands.add_parser("pack")
    pack_cmd.add_argument("product", type=Path)
    pack_cmd.add_argument("output", type=Path)
    verify_cmd = commands.add_parser("verify")
    verify_cmd.add_argument("bundle", type=Path)
    args = parser.parse_args()
    if args.command == "pack":
        pack(args.product, args.output)
        verify(args.output)
    else:
        verify(args.bundle)


if __name__ == "__main__":
    main()
