#!/usr/bin/env python3
"""Build the pinned Waybar fork as an independent native ARM64 component."""
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile

from artifact_download import download

from pipeline_cache import Cache, digest


TRAY_REVISION = '00f37a04a66e2134a95e19cc0ecec21459fd4e76'
TRAY_SHA256 = '793548d092b27b92cd44b666baa8f6cddebe7bf4ba353dd23958a8fd258da5c8'
RUST_VERSION = '1.90.0'


def run(*args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    framework = Path(__file__).resolve().parents[2]
    if os.environ.get('GITHUB_ACTIONS') != 'true' or platform.machine() not in ('aarch64', 'arm64'):
        raise RuntimeError('Waybar compilation requires the ARM64 GitHub runner')
    source = framework/'third_party/waybar'
    run('rustup', 'toolchain', 'install', RUST_VERSION, '--profile', 'minimal', '--no-self-update')
    inputs = dict(recipe=digest(__file__), tray=TRAY_SHA256, rust=RUST_VERSION,
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
            '-Dauto_features=disabled', '-Dniri=false', '-Ddbusmenu-gtk=enabled',
            '--force-fallback-for=fmt,spdlog,jsoncpp,date,gtk-layer-shell', env=env)
        run('meson', 'compile', '-C', work, '-j4', env=env)
        binary = output/'rootfs/usr/bin/waybar'
        binary.parent.mkdir(parents=True)
        shutil.copy2(work/'waybar', binary)
        run('strip', '--strip-unneeded', binary)
        # Standard XEmbed -> SNI bridge; no Qt runtime, Wine patch or window rule.
        tray_work = work/'xembsni'
        tray_work.mkdir()
        archive = download(f'https://codeload.github.com/jmylchreest/xembsni/tar.gz/{TRAY_REVISION}',
                           tray_work/'source.tar.gz', TRAY_SHA256)
        with tarfile.open(archive) as stream:
            stream.extractall(tray_work, filter='data')
        tray_source = tray_work/f'xembsni-{TRAY_REVISION}'
        run('cargo', '+'+RUST_VERSION, 'build', '--locked', '--release', '--bin', 'xembsni',
            '-j4', '--manifest-path', tray_source/'Cargo.toml',
            env=dict(os.environ, CARGO_PROFILE_RELEASE_LTO='thin', CARGO_PROFILE_RELEASE_CODEGEN_UNITS='1'))
        tray_binary = binary.with_name('xembsni')
        shutil.copy2(tray_source/'target/release/xembsni', tray_binary)
        run('strip', '--strip-unneeded', tray_binary)
        tray_license = output/'rootfs/usr/share/doc/xembsni'
        tray_license.mkdir(parents=True)
        for name in ('Cargo.toml', 'README.md'):
            shutil.copy2(tray_source/name, tray_license/name)
        metadata = json.loads(subprocess.check_output(
            ['cargo', '+'+RUST_VERSION, 'metadata', '--locked', '--format-version=1',
             '--filter-platform=aarch64-unknown-linux-gnu', '--manifest-path', str(tray_source/'Cargo.toml')], text=True))
        notices = []
        for package in metadata['packages']:
            directory = Path(package['manifest_path']).parent
            destination = tray_license/(package['name']+'-'+package['version'])
            destination.mkdir(exist_ok=True)
            for path in directory.iterdir():
                if path.is_file() and path.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE', 'AUTHORS')):
                    shutil.copy2(path, destination/path.name)
            notices.append({key: package.get(key) for key in ('name', 'version', 'license', 'authors', 'repository')})
        (tray_license/'licenses.json').write_text(json.dumps(notices, indent=2)+'\n')
        (tray_license/'SOURCE').write_text(
            f'https://github.com/jmylchreest/xembsni/tree/{TRAY_REVISION}\n')
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
