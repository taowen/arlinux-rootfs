#!/usr/bin/env python3
"""Package the shared Mesa provider payload for Arch Linux ARM's pacman."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[2]


def build():
    packages = sorted((REPO / 'build/linux/mesa-debs').glob('*.deb'))
    if len(packages) != 5:
        raise RuntimeError('Build the five shared Mesa provider DEBs first')
    digest = hashlib.sha256(Path(__file__).read_bytes())
    for package in packages:
        digest.update(package.name.encode())
        digest.update(subprocess.check_output(['dpkg-deb', '-f', str(package), 'Version']))
    version = '1:' + (REPO / 'third_party/mesa/VERSION').read_text().strip().replace('-devel', '.devel')
    version += '.arlinux.' + digest.hexdigest()[:12] + '-1'
    dependencies = {
        'libc6': 'glibc', 'libgcc-s1': 'libgcc', 'libstdc++6': 'libstdc++',
        'libdrm2': 'libdrm', 'libexpat1': 'expat', 'zlib1g': 'zlib',
        'libx11-6': 'libx11', 'libx11-xcb1': 'libx11', 'libxext6': 'libxext',
        'libxxf86vm1': 'libxxf86vm', 'libxshmfence1': 'libxshmfence',
        'libwayland-client0': 'wayland', 'libglvnd0': 'libglvnd',
        'libvulkan1': 'vulkan-icd-loader',
    }
    requires = set()
    with tempfile.TemporaryDirectory(prefix='arlinux-mesa-arch-') as temp:
        root = Path(temp)
        for package in packages:
            deps = subprocess.check_output(['dpkg-deb', '-f', str(package), 'Depends'], text=True)
            for dep in deps.split(','):
                name = dep.strip().split()[0]
                if name.startswith('libxcb'):
                    requires.add('libxcb')
                elif name in dependencies:
                    requires.add(dependencies[name])
                elif name not in ('mesa-libgallium', 'libgbm1', 'libgl1-mesa-dri'):
                    raise RuntimeError(f'Unmapped Arch dependency: {name}')
            subprocess.run(['dpkg-deb', '-x', str(package), temp], check=True)
        multiarch = root / 'usr/lib/aarch64-linux-gnu'
        for link in multiarch.rglob('*'):
            if link.is_symlink():
                dest = root / 'usr/lib' / link.relative_to(multiarch)
                dest.parent.mkdir(parents=True, exist_ok=True)
                target = Path('mesa') / link.resolve().relative_to(root / 'usr/lib/mesa')
                dest.symlink_to(('..' if dest.parent.name == 'dri' else '.') + '/' + str(target))
        shutil.rmtree(multiarch)
        licenses = root / 'usr/share/licenses/mesa'
        licenses.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(root / 'usr/share/doc/mesa-libgallium', licenses)
        shutil.rmtree(root / 'usr/share/doc')
        size = sum(p.stat().st_size for p in root.rglob('*') if p.is_file() and not p.is_symlink())
        (root / '.PKGINFO').write_text(
            f'pkgname = mesa\npkgver = {version}\npkgdesc = ARLinux shared Zink Mesa providers\n'
            'url = https://github.com/taowen/arlinux-rootfs\n'
            f'builddate = {int(time.time())}\npackager = ARLinux <contact@arlinux.com>\n'
            f'size = {size}\narch = aarch64\nlicense = MIT AND BSD-3-Clause AND SGI-B-2.0\n'
            f'provides = opengl-driver\nprovides = mesa-libgl={version}\n'
            'provides = libgbm.so=1-64\nprovides = libEGL_mesa.so=0-64\n'
            'provides = libGLX_mesa.so=0-64\n'
            + ''.join(f'depend = {name}\n' for name in sorted(requires)))
        output = REPO / 'build/linux/mesa-arch'
        output.mkdir(parents=True, exist_ok=True)
        subprocess.run(['tar', '--owner=0', '--group=0', '--numeric-owner', '--zstd',
                        '-cf', str(output / 'mesa-aarch64.pkg.tar.zst'), '-C', temp,
                        '.PKGINFO', 'usr'], check=True)
    print(version)


if __name__ == '__main__':
    build()
