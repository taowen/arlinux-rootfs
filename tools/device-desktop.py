#!/usr/bin/env python3
"""Prepare a disposable device-build ZIP, or seal its rootfs snapshot for offline use.

No ARM emulation is used here. Install the preparation ZIP in a fresh build-only
instance, let its installer finish, stop the instance, then export its rootfs.
Never supply a personal instance: home, credentials and application state must
not be used as build inputs.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import zipfile

from bundle import verify


def rewrite(seed: Path, output: Path, replacements: dict[str, bytes | Path]) -> None:
    verify(seed)
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_suffix('.zip.part')
    with zipfile.ZipFile(seed) as source, zipfile.ZipFile(pending, 'w') as target:
        manifest = json.loads(source.read('manifest.json'))
        if manifest['distributionId'] not in ('debian', 'yibu'):
            raise ValueError('The device desktop installer targets Debian-based offline desktops')
        for name in sorted(set(manifest['files']) | set(replacements)):
            value = replacements.get(name)
            digest = hashlib.sha256()
            stream = (value.open('rb') if isinstance(value, Path) else
                      io.BytesIO(value) if isinstance(value, bytes) else source.open(name))
            with stream, target.open(name, 'w', force_zip64=True) as dest:
                while block := stream.read(1024 * 1024):
                    digest.update(block)
                    dest.write(block)
            manifest['files'][name] = digest.hexdigest()
        target.writestr('manifest.json', json.dumps(manifest, indent=2) + '\n')
    verify(pending)
    pending.replace(output)


def prepare(seed: Path, output: Path) -> None:
    with zipfile.ZipFile(seed) as archive:
        if 'offline-desktop' in archive.namelist():
            raise ValueError('Expected an unconfigured seed ZIP, not an offline desktop')
        archive.getinfo('guest/build-desktop.sh')
    rewrite(seed, output, {'guest/first-boot.sh': b'''#!/bin/sh
set -eu
/bin/sh /usr/lib/arlinux/guest/build-desktop.sh
''', 'device-build': b'1\n'})


def seal(seed: Path, snapshot: Path, output: Path) -> None:
    # Start with an unconfigured seed so no preparation entry point can ship.
    with zipfile.ZipFile(seed) as archive:
        if 'device-build' in archive.namelist():
            raise ValueError('Seal against the original seed, not the preparation ZIP')
        final_first_boot = archive.read('guest/first-boot.sh')
        gpu_files = set()
        for overlay in ('gpu-qualcomm.tar.zst', 'gpu-generic.tar.zst'):
            result = subprocess.run(['zstd', '-dc'], input=archive.read(overlay),
                                    stdout=subprocess.PIPE, check=True)
            with tarfile.open(fileobj=io.BytesIO(result.stdout)) as tar:
                gpu_files.update(m.name.removeprefix('./') for m in tar if not m.isdir())
    dropped = {
        'etc/machine-id', 'etc/resolv.conf', 'var/lib/dbus/machine-id',
        'etc/pulse/client.conf.d/arlinux.conf', 'usr/bin/arlinux-app', 'usr/bin/sudo',
        'usr/share/applications/arlinux-app.desktop',
        'usr/share/arlinux/desktop-packages-installed',
    } | gpu_files
    prefixes = ('var/cache/', 'var/log/', 'var/tmp/', 'var/lib/apt/lists/',
                'var/run', 'var/lock', 'usr/lib/mesa/', 'usr/lib/hybris/')
    allowed = {'etc', 'usr', 'opt', 'var', 'bin', 'sbin', 'lib', 'lib64'}
    seen = set()
    mesa_packages = set()
    with tempfile.TemporaryDirectory(prefix='arlinux-seal-') as temp:
        compressed = Path(temp) / 'rootfs.tar.zst'
        with compressed.open('wb') as dest:
            compressor = subprocess.Popen(['zstd', '-T4', '-19', '-c'], stdin=subprocess.PIPE, stdout=dest)
            try:
                with tarfile.open(snapshot, 'r|') as source, tarfile.open(fileobj=compressor.stdin, mode='w|') as target:
                    for member in source:
                        name = member.name.removeprefix('./').rstrip('/')
                        if '..' in name.split('/') or name.startswith('/'):
                            raise ValueError('Unsafe snapshot entry: ' + name)
                        if name == 'opt/OpenCode' or name.startswith('opt/OpenCode/'):
                            raise ValueError('OpenCode must be installed on demand, not exported in the offline desktop')
                        if name.split('/')[0] not in allowed or name in dropped or name.startswith(prefixes):
                            continue
                        if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                            continue
                        if member.issym() and member.linkname.startswith(('/data/', '/sdcard/')):
                            raise ValueError('Device-specific symlink: ' + name)
                        member.name = './' + name
                        member.uid = member.gid = member.mtime = 0
                        member.uname = member.gname = ''
                        member.pax_headers = {}
                        body = source.extractfile(member) if member.isfile() else None
                        if name in ('etc/passwd', 'etc/group', 'etc/passwd-', 'etc/group-'):
                            data = b''.join(line for line in body if not line.startswith(b'arlinux:'))
                            body = io.BytesIO(data)
                            member.size = len(data)
                        elif name == 'usr/lib/arlinux/guest/first-boot.sh':
                            body = io.BytesIO(final_first_boot)
                            member.size = len(final_first_boot)
                        elif name == 'var/lib/dpkg/status':
                            data = body.read()
                            for block in data.decode().split('\n\n'):
                                fields = dict(line.split(': ', 1) for line in block.splitlines()
                                              if ': ' in line and not line.startswith(' '))
                                if (fields.get('Status') == 'install ok installed'
                                        and '+arlinux.' in fields.get('Version', '')):
                                    mesa_packages.add(fields.get('Package'))
                            body = io.BytesIO(data)
                        elif name == 'usr/share/arlinux/offline-desktop':
                            if body.read() != b'1\n':
                                raise ValueError('Device preparation did not finish')
                            body = io.BytesIO(b'1\n')
                        target.addfile(member, body)
                        seen.add(name)
                    for name in ('etc/machine-id',):
                        member = tarfile.TarInfo('./' + name)
                        member.mode = 0o644
                        target.addfile(member, io.BytesIO())
                compressor.stdin.close()
                if compressor.wait() != 0:
                    raise RuntimeError('rootfs compression failed')
            finally:
                if compressor.poll() is None:
                    compressor.kill()
                    compressor.wait()
        required = {'usr/share/arlinux/offline-desktop', 'usr/share/arlinux/packages.tsv',
                    'usr/bin/arlinux-opencode', 'usr/bin/thunar', 'usr/bin/mousepad'}
        if not required <= seen:
            raise ValueError('Incomplete device desktop: ' + ', '.join(sorted(required - seen)))
        providers = {'mesa-libgallium', 'libgbm1', 'libgl1-mesa-dri', 'libglx-mesa0', 'libegl-mesa0'}
        if not providers <= mesa_packages:
            raise ValueError('Device desktop is missing ARLinux Mesa provider packages: ' +
                             ', '.join(sorted(providers - mesa_packages)))
        with compressed.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        rewrite(seed, output, {'rootfs.tar.zst': compressed,
                              'rootfs-seed-id': (digest + '\n').encode(),
                              'offline-desktop': b'1\n'})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    preparation = sub.add_parser('prepare')
    preparation.add_argument('seed', type=Path)
    preparation.add_argument('output', type=Path)
    sealing = sub.add_parser('seal')
    sealing.add_argument('seed', type=Path)
    sealing.add_argument('snapshot', type=Path)
    sealing.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.command == 'prepare':
        prepare(args.seed, args.output)
    else:
        seal(args.seed, args.snapshot, args.output)
