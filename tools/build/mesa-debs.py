#!/usr/bin/env python3
"""Package the shared Zink stack as real Debian Mesa provider packages.

Keep Debian's GLVND dispatch packages. Replace only its Mesa providers, including
GBM, so APT does not install a second Gallium with LLVM and desktop GPU drivers.
The GPU overlay restores these same package-owned files when an instance starts.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
MESA = REPO / 'build/linux/mesa/lib'
OUTPUT = REPO / 'build/linux/mesa-debs'
MULTIARCH = 'usr/lib/aarch64-linux-gnu'


def build():
    gallium, = MESA.glob('libgallium-*.so')
    libraries = {
        'mesa-libgallium': [gallium.name],
        'libgbm1': ['libgbm.so.1.0.0', 'gbm/dri_gbm.so'],
        'libgl1-mesa-dri': ['dri/libdril_dri.so'],
        'libglx-mesa0': ['libGLX_mesa.so.0.0.0'],
        'libegl-mesa0': ['libEGL_mesa.so.0.0.0'],
    }
    digest = hashlib.sha256()
    digest.update(Path(__file__).read_bytes())
    for name in sorted(name for names in libraries.values() for name in names):
        digest.update(name.encode())
        digest.update((MESA / name).read_bytes())
    upstream = (REPO / 'third_party/mesa/VERSION').read_text().strip().replace('-devel', '~devel')
    version = '1:' + upstream + '+arlinux.' + digest.hexdigest()[:12]
    same = lambda name: f'{name} (= {version})'
    dependencies = {
        'mesa-libgallium': {'libvulkan1'},
        'libgbm1': set(),
        'libgl1-mesa-dri': {same('mesa-libgallium'), same('libgbm1')},
        'libglx-mesa0': {'libglvnd0', same('libgl1-mesa-dri')},
        'libegl-mesa0': {'libglvnd0'},
    }
    private = {gallium.name: same('mesa-libgallium'), 'libgbm.so.1': same('libgbm1')}
    for package, names in libraries.items():
        for name in names:
            elf = subprocess.check_output(['readelf', '-d', str(MESA / name)], text=True)
            for needed in re.findall(r'NEEDED.*\[(.*?)\]', elf):
                if needed in private:
                    dependencies[package].add(private[needed])
                    continue
                owners = subprocess.check_output(['dpkg-query', '-S',
                    '/usr/lib/aarch64-linux-gnu/' + needed], text=True).splitlines()
                owner, = {line.split(': ')[0].split(':')[0] for line in owners}
                dependencies[package].add('libc6 (>= 2.38)' if owner == 'libc6' else owner)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for stale in OUTPUT.glob('*.deb'):
        stale.unlink()
    with tempfile.TemporaryDirectory(prefix='arlinux-mesa-debs-') as temp:
        for package, names in libraries.items():
            root = Path(temp) / package
            for name in names:
                dest = root / 'usr/lib/mesa' / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(MESA / name, dest)
                subprocess.run(['aarch64-linux-gnu-strip', '--strip-unneeded', str(dest)], check=True)
                origin = dest.parent
                relative = (root / 'usr/lib/mesa').relative_to(root)
                rpath = '$ORIGIN/' + os.path.relpath(root / relative, origin)
                subprocess.run(['patchelf', '--set-rpath', rpath, str(dest)], check=True)
            links = {
                'mesa-libgallium': {gallium.name: '../mesa/' + gallium.name},
                'libgbm1': {'libgbm.so.1': '../mesa/libgbm.so.1.0.0'},
                'libglx-mesa0': {'libGLX_mesa.so.0': '../mesa/libGLX_mesa.so.0.0.0'},
                'libegl-mesa0': {'libEGL_mesa.so.0': '../mesa/libEGL_mesa.so.0.0.0'},
            }.get(package, {})
            for name, target in links.items():
                path = root / MULTIARCH / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.symlink_to(target)
            if package == 'libgl1-mesa-dri':
                for name in ('libdril_dri.so', 'zink_dri.so', 'swrast_dri.so'):
                    path = root / MULTIARCH / 'dri' / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.symlink_to('../../mesa/dri/libdril_dri.so')
            if package == 'libegl-mesa0':
                path = root / 'usr/share/glvnd/egl_vendor.d/50_mesa.json'
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_mesa.so.0"}}\n')
            control = root / 'DEBIAN'
            control.mkdir()
            (control / 'control').write_text(
                f'Package: {package}\nVersion: {version}\nArchitecture: arm64\n'
                'Maintainer: ARLinux <contact@arlinux.com>\nSection: libs\nPriority: optional\nMulti-Arch: same\n'
                f'Depends: {", ".join(sorted(dependencies[package]))}\n'
                f'Description: ARLinux Mesa provider ({package})\n'
                ' Zink over the Android Vulkan driver, without LLVM or desktop GPU drivers.\n')
            (control / 'triggers').write_text('activate-noawait ldconfig\n')
            shlibs = {
                'libgbm1': 'libgbm 1 libgbm1',
                'libglx-mesa0': 'libGLX_mesa 0 libglx-mesa0',
                'libegl-mesa0': 'libEGL_mesa 0 libegl-mesa0',
            }.get(package)
            if shlibs:
                (control / 'shlibs').write_text(shlibs + f' (>= {version})\n')
            copyright = root / f'usr/share/doc/{package}/copyright'
            copyright.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / 'third_party/mesa/docs/license.rst', copyright)
            shutil.copytree(REPO / 'third_party/mesa/licenses', copyright.parent / 'licenses')
            subprocess.run(['dpkg-deb', '--root-owner-group', '-Zxz', '--build', str(root),
                            str(OUTPUT / f'{package}_arm64.deb')], check=True)
    print(version)


if __name__ == '__main__':
    build()
