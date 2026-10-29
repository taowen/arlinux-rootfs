#!/usr/bin/env python3
"""Install repository-hosted official ARM64 DEBs on first use, then launch them."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import desktop_bus
import shared

RELEASE = 'https://api.github.com/repos/taowen/arlinux-rootfs/releases/tags/popular-apps'
DOWNLOAD = 'https://github.com/taowen/arlinux-rootfs/releases/download/popular-apps/'
APPS = {
    'qq': ('linuxqq', '/opt/QQ/qq', 'qq.desktop', ['--force-renderer-accessibility']),
    'wechat': ('wechat', '/usr/bin/wechat', 'wechat.desktop', []),
}


def installed(package, binary):
    status = subprocess.run(['dpkg-query', '-W', '-f=${Status}', package],
                            capture_output=True, text=True)
    return status.returncode == 0 and status.stdout == 'install ok installed' and os.access(binary, os.X_OK)


def install(name, package):
    asset_name = name + '-arm64.deb'
    release = json.loads(shared.github_metadata(RELEASE))
    asset = next((item for item in release['assets'] if item['name'] == asset_name), None)
    if asset is None:
        raise RuntimeError(f'Installer unavailable: {asset_name}')
    digest = asset.get('digest', '')
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', digest) or asset['browser_download_url'] != DOWNLOAD + asset_name:
        raise ValueError('Invalid Linux application download metadata')
    installer = shared.download(DOWNLOAD + asset_name, digest.removeprefix('sha256:'), '.deb')
    fields = subprocess.check_output(['dpkg-deb', '-f', str(installer), 'Package', 'Architecture'], text=True)
    if fields.splitlines() != [f'Package: {package}', 'Architecture: arm64']:
        raise ValueError('Installer package or architecture does not match the application')
    env = dict(os.environ, DEBIAN_FRONTEND='noninteractive')
    subprocess.run(['sudo', 'apt-get', 'update'], check=True, env=env)
    subprocess.run(['sudo', 'apt-get', 'install', '-y', '--no-install-recommends', str(installer)],
                   check=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('application', choices=APPS)
    parser.add_argument('arguments', nargs='*')
    args = parser.parse_args()
    if not shutil.which('apt-get') or subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip() != 'arm64':
        raise RuntimeError('These installers require an ARM64 Debian-family instance')
    package, binary, vendor_entry, flags = APPS[args.application]
    cache = Path(os.environ.get('XDG_CACHE_HOME', Path.home()/'.cache'))/'arlinux/popular-apps'
    cache.mkdir(parents=True, exist_ok=True)
    with shared.locked(cache/'install.lock'):
        if not installed(package, binary):
            print(f'Installing {args.application}…', flush=True)
            install(args.application, package)
        if not installed(package, binary):
            raise RuntimeError('Application installation did not complete')
    # Keep the on-demand entry as the single Apps entry after APT installs the
    # vendor shortcut. Preserve any existing user override.
    applications = Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'applications'
    applications.mkdir(parents=True, exist_ok=True)
    try:
        with (applications/vendor_entry).open('x') as entry:
            entry.write('[Desktop Entry]\nType=Application\nHidden=true\n')
    except FileExistsError:
        pass
    env = desktop_bus.environment(os.environ)
    with (cache/(args.application + '.log')).open('ab') as log:
        subprocess.Popen([binary, *flags, *args.arguments], env=env,
                         stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                         start_new_session=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Linux application: {error}\nClose this window and launch the app again to retry.',
              file=sys.stderr, flush=True)
        if sys.stdin.isatty():
            try:
                input('\nPress Enter to close this window. ')
            except (EOFError, KeyboardInterrupt):
                pass
        sys.exit(1)
