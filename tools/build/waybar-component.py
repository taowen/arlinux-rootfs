#!/usr/bin/env python3
"""Build the pinned Waybar fork as an independent native ARM64 component."""
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

from pipeline_cache import Cache, digest


def run(*args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    framework = Path(__file__).resolve().parents[2]
    if os.environ.get('GITHUB_ACTIONS') != 'true' or platform.machine() not in ('aarch64', 'arm64'):
        raise RuntimeError('Waybar compilation requires the ARM64 GitHub runner')
    source = framework/'third_party/waybar'
    inputs = dict(recipe=digest(__file__),
                  source=subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip(),
                  compiler=subprocess.check_output(['c++', '--version'], text=True),
                  meson=subprocess.check_output(['meson', '--version'], text=True),
                  packages=subprocess.check_output(['dpkg-query', '-W'], text=True))
    cache = Cache(framework/'build/pipeline')

    def build(output):
        work = framework/'build/linux/waybar-build'
        if work.exists():
            shutil.rmtree(work)
        env = dict(os.environ, CC='ccache cc', CXX='ccache c++')
        # Pin and statically link small upstream dependencies through Waybar's
        # checksum-verified wraps. Do not bind Debian guests to Ubuntu's fmt,
        # spdlog or jsoncpp sonames, or ship another copy of GTK and glibc.
        run('meson', 'setup', work, source, '--prefix=/usr', '--buildtype=release',
            '-Dauto_features=disabled', '-Dniri=false',
            '--force-fallback-for=fmt,spdlog,jsoncpp,date,gtk-layer-shell', env=env)
        run('meson', 'compile', '-C', work, '-j4', env=env)
        binary = output/'rootfs/usr/bin/waybar'
        binary.parent.mkdir(parents=True)
        shutil.copy2(work/'waybar', binary)
        run('strip', '--strip-unneeded', binary)
        licenses = output/'rootfs/usr/share/doc/arlinux-waybar'
        for directory in [source, *sorted((source/'subprojects').glob('*'))]:
            if not directory.is_dir():
                continue
            for item in sorted(directory.iterdir()):
                if item.is_file() and item.name.upper().startswith(('LICENSE', 'COPYING')):
                    target = licenses/('waybar' if directory == source else directory.name)/item.name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(item, target)

    output = cache.stage('linux-waybar', inputs, build)
    descriptor = framework/'build/artifacts/components/linux-waybar.json'
    descriptor.parent.mkdir(parents=True, exist_ok=True)
    descriptor.write_text(json.dumps(dict(format=1, stage='linux-waybar', output=str(output),
                                          records=cache.records), sort_keys=True)+'\n')


if __name__ == '__main__':
    main()
