#!/usr/bin/env python3
"""Validate an Arlinux distribution recipe without third-party Python modules."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys


IDENTIFIER = re.compile(r"[a-z][a-z0-9-]{0,63}\Z")
ENVIRONMENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
GLIBC_VERSION = re.compile(r"[0-9]+\.[0-9]+\Z")


def fail(message: str) -> None:
    raise ValueError(message)


def relative_path(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"{field} entries must be non-empty strings")
    path = Path(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        fail(f"{field} contains an unsafe path: {value!r}")
    return value


def load_object(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read {path}: {error}")
    if not isinstance(value, dict):
        fail(f"{path} must contain a JSON object")
    return value


def validate(directory: Path) -> None:
    directory = directory.resolve()
    identifier = directory.name
    if not IDENTIFIER.fullmatch(identifier):
        fail(f"invalid distribution directory name: {identifier!r}")

    product_path = directory / "product.json"
    product = load_object(product_path)
    allowed = {
        "$schema", "name", "compositor", "glibcVersion", "libraryDirectories",
        "requiredFiles", "environment",
    }
    unknown = sorted(set(product) - allowed)
    if unknown:
        fail("unknown product.json fields: " + ", ".join(unknown))
    if not isinstance(product.get("name"), str) or not product["name"].strip():
        fail("product.json name must be a non-empty string")
    if product.get("compositor", "anlabwc") not in ("anlabwc", "hyprland"):
        fail("product.json compositor must be anlabwc or hyprland")
    glibc = product.get("glibcVersion")
    if not isinstance(glibc, str) or not GLIBC_VERSION.fullmatch(glibc):
        fail("product.json glibcVersion must look like 2.43")
    for field in ("libraryDirectories", "requiredFiles"):
        values = product.get(field)
        if not isinstance(values, list) or not values:
            fail(f"product.json {field} must be a non-empty array")
        normalized = [relative_path(value, field) for value in values]
        if len(normalized) != len(set(normalized)):
            fail(f"product.json {field} contains duplicates")
    environment = product.get("environment", {})
    if not isinstance(environment, dict) or any(
        not ENVIRONMENT.fullmatch(key) or not isinstance(value, str)
        for key, value in environment.items()
    ):
        fail("product.json environment must map environment names to strings")

    recipe = Path(__file__).resolve().parents[1] / "runtime/glibc" / glibc / "recipe.env"
    if not recipe.is_file():
        fail(f"arlinux-rootfs has no glibc {glibc} recipe")
    seed = directory / "tools/seed.sh"
    if not seed.is_file() or seed.stat().st_mode & 0o111 == 0:
        fail("tools/seed.sh must exist and be executable")
    guest = directory / "guest"
    if not guest.is_dir() or not (guest / "first-boot.sh").is_file():
        fail("guest/first-boot.sh is required")
    policy = directory / "native/product-policy.h"
    if not policy.is_file():
        fail("native/product-policy.h is required")
    profile = directory / "profile.json"
    if profile.exists():
        data = load_object(profile)
        if data.get("schemaVersion") != 3 or not isinstance(data.get("launch"), dict):
            fail("profile.json must be a schemaVersion 3 launch profile")

    print(f"OK distribution {identifier}: {product['name']} (glibc {glibc})")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} DISTRIBUTION_DIRECTORY")
    try:
        validate(Path(sys.argv[1]))
    except ValueError as error:
        raise SystemExit(f"error: {error}") from error
