#!/usr/bin/env python3
"""Build the unmodified upstream IBus Wayland adapter on its native ARM64 runner."""
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile

from artifact_download import download
from pipeline_cache import Cache, digest, inventory

VERSION = '1.5.35-rc1'
SOURCE_SHA256 = '3963bfe45bfb73ff967cf3ae2542a4f479e6dd9506304a57b9d844d1ae2f49cb'
PREFIX = '/usr/lib/arlinux/ibus'


def run(*args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    framework = Path(__file__).resolve().parents[2]
    if os.environ.get('GITHUB_ACTIONS') != 'true' or platform.machine() not in ('aarch64', 'arm64'):
        raise RuntimeError('IBus compilation requires the ARM64 GitHub runner')
    inputs = dict(recipe=digest(__file__), source=SOURCE_SHA256, version=VERSION,
                  compiler=subprocess.check_output(['cc', '--version'], text=True),
                  packages=subprocess.check_output(['dpkg-query', '-W'], text=True))
    cache = Cache(framework/'build/pipeline')
    prefix = framework/'build/linux/ibus'
    receipt = framework/'build/linux/ibus.outputs.json'

    def build(output):
        work = framework/'build/linux/ibus-build'
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)
        archive = download(f'https://github.com/ibus/ibus/releases/download/{VERSION}/ibus-{VERSION}.tar.xz',
                           work/'source.tar.xz', SOURCE_SHA256)
        with tarfile.open(archive) as source:
            source.extractall(work, filter='data')
        source = work/f'ibus-{VERSION}'
        run('gtkdocize', '--copy', cwd=source)
        run('autoreconf', '-fi', cwd=source)
        build_dir = work/'build'
        build_dir.mkdir()
        env = dict(os.environ, CC='ccache cc', CXX='ccache c++')
        run(source/'configure', '--prefix='+PREFIX, '--libdir='+PREFIX+'/lib',
            '--disable-gtk2', '--disable-gtk4', '--disable-xim', '--disable-engine',
            '--disable-setup', '--with-python=/usr/bin/python3',
            '--with-python-overrides-dir='+PREFIX+'/share/python/gi/overrides',
            '--with-gtk3-im-module-dir='+PREFIX+'/lib/gtk-3.0/immodules',
            '--disable-appindicator',
            '--disable-libnotify', '--disable-emoji-dict', '--disable-unicode-dict',
            '--disable-tests', '--disable-install-tests', '--disable-gtk-doc',
            '--disable-systemd-services', '--enable-wayland', cwd=build_dir, env=env)
        run('make', '-j4', cwd=build_dir, env=env)
        if prefix.exists():
            shutil.rmtree(prefix)
        run('make', 'install', 'DESTDIR='+str(prefix), cwd=build_dir, env=env)
        # Keep the updated adapter and its ABI-matched IBus client library in
        # a private prefix. Distribution-owned daemon/GI/GTK modules stay intact.
        # The adapter uses the existing session bus and hosted input engine.
        for path in (prefix/PREFIX.lstrip('/')).rglob('*'):
            if path.is_file() and not path.is_symlink():
                with path.open('rb') as stream:
                    elf = stream.read(4) == b'\x7fELF'
                if elf:
                    run('strip', '--strip-unneeded', path)
                    relative = os.path.relpath(prefix/PREFIX.lstrip('/')/'lib', path.parent)
                    run('patchelf', '--set-rpath', '$ORIGIN/'+relative, path)
        license_dir = prefix/PREFIX.lstrip('/')/'share/licenses/ibus'
        license_dir.mkdir(parents=True)
        shutil.copy2(source/'COPYING', license_dir/'COPYING')
        receipt.write_text(json.dumps(dict(format=1, inputs=inputs, outputs=inventory(prefix)), sort_keys=True)+'\n')
        output.mkdir()
        (output/'build-state.json').write_text(json.dumps(dict(prefix=str(prefix), receipt=str(receipt)))+'\n')

    output = cache.stage('linux-ibus', inputs, build)
    descriptor = framework/'build/artifacts/components/linux-ibus.json'
    descriptor.parent.mkdir(parents=True, exist_ok=True)
    descriptor.write_text(json.dumps(dict(format=1, stage='linux-ibus', output=str(output), records=cache.records), sort_keys=True)+'\n')


if __name__ == '__main__':
    main()
