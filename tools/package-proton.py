#!/usr/bin/env python3
"""Package a built ARM64 Proton redist for arlinux-windows (not for Steam).

This does not build Proton or mix components from different Wine versions.
Pass the complete redist produced by its matching source build.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile


def git(source, *arguments):
    return subprocess.check_output(['git', '-C', str(source), *arguments])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--redist', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path, help='Matching Proton source checkout')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--development', action='store_true', help='Mark an experimental local build')
    parser.add_argument('--wine-source-archive', type=Path, help='Exact prepared Wine source, including Android fixes')
    parser.add_argument('--integration-source', type=Path, help='ARLinux GE fork checkout')
    args = parser.parse_args()
    source, redist = args.source.resolve(), args.redist.resolve()
    wine_diff = git(source/'wine', 'diff', 'HEAD')
    if wine_diff and not args.development:
        raise ValueError('Release bundles require committed Wine sources; use --development for experiments')
    module_path = Path(__file__).resolve().parents[1]/'runtime/tools/windows.py'
    spec = importlib.util.spec_from_file_location('windows', module_path)
    windows = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(windows)
    native_files = ('files/bin-arm64/wine', 'files/bin-arm64/wineserver',
                    'files/lib/wine/aarch64-unix/ntdll.so',
                    'files/lib/wine/aarch64-unix/nsiproxy.so')
    pe_files = tuple(f'files/lib/wine/{arch}-windows/iphlpapi.dll'
                     for arch in ('aarch64', 'i386', 'x86_64'))
    files = ('proton',) + native_files + pe_files
    manifest = {'format': 1, 'architecture': 'aarch64',
                'development': args.development,
                'proton_commit': git(source, 'rev-parse', 'HEAD').decode().strip(),
                'wine_commit': git(source/'wine', 'rev-parse', 'HEAD').decode().strip(),
                'wine_diff_sha256': hashlib.sha256(wine_diff).hexdigest(),
                'files': {name: windows.digest(redist/name) for name in files}}
    if args.wine_source_archive:
        manifest['wine_source_archive'] = args.wine_source_archive.name
        manifest['wine_source_sha256'] = windows.digest(args.wine_source_archive)
    if args.integration_source:
        manifest['integration_commit'] = git(args.integration_source, 'rev-parse', 'HEAD').decode().strip()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Do not write a manifest into the caller's redist or alter its files.
    with tempfile.TemporaryDirectory(prefix='proton-package-', dir=args.output.parent) as temporary:
        stage = Path(temporary)
        metadata = stage/'arlinux-runtime.json'
        metadata.write_text(json.dumps(manifest, indent=2)+'\n')
        # Validate native ELF architecture before doing the expensive compression.
        for name in native_files:
            with (redist/name).open('rb') as binary:
                header = binary.read(20)
            if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\xb7\x00':
                raise ValueError(f'Not an ARM64 Proton redist: {name}')
        for name, machine in zip(pe_files, (0xaa64, 0x14c, 0x8664)):
            with (redist/name).open('rb') as binary:
                header = binary.read(64)
                if header[:2] != b'MZ':
                    raise ValueError(f'Not a PE module: {name}')
                binary.seek(int.from_bytes(header[60:64], 'little'))
                pe = binary.read(6)
            if pe[:4] != b'PE\0\0' or int.from_bytes(pe[4:6], 'little') != machine:
                raise ValueError(f'Wrong PE architecture: {name}')
        archive = stage/'runtime.tar.gz'
        with tarfile.open(archive, 'w:gz', compresslevel=3) as bundle:
            for entry in sorted(redist.iterdir()):
                if entry.name != metadata.name:
                    bundle.add(entry, arcname=entry.name)
            bundle.add(metadata, arcname=metadata.name)
        archive.replace(args.output)
    checksum = windows.digest(args.output)
    args.output.with_name(args.output.name+'.sha256').write_text(f'{checksum}  {args.output.name}\n')
    print(f'{checksum}  {args.output}')


if __name__ == '__main__':
    main()
