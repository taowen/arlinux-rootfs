#!/usr/bin/env python3
"""Install repository-hosted official ARM64 DEBs on first use, then launch them."""
import argparse
from dataclasses import dataclass, field
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import desktop_bus
import shared

DOWNLOAD = 'https://github.com/taowen/arlinux-rootfs/releases/download/popular-apps/'


@dataclass(frozen=True)
class Application:
    package: str
    binary: str
    desktop: str
    flags: tuple[str, ...] = ()
    dependencies: tuple[str, ...] = ()
    environment: dict[str, str] = field(default_factory=dict)


APPS = {
    'grok-bot': Application('grok-bot', '/usr/bin/grok-bot', 'grok-bot.desktop',
                            flags=('--force-renderer-accessibility',)),
    'qq': Application('linuxqq', '/opt/QQ/qq', 'qq.desktop', flags=('--force-renderer-accessibility',)),
    # The vendor DEB omits NSS. Its Qt build scales the Wayland surface but
    # leaves its painted controls at 1x, producing transparent input regions.
    'wechat': Application('wechat', '/usr/bin/wechat', 'wechat.desktop',
                          dependencies=('libnss3',),
                          environment={'QT_ENABLE_HIGHDPI_SCALING': '0'}),
}


def installed(app: Application) -> bool:
    packages = (app.package, *app.dependencies)
    status = subprocess.run(['dpkg-query', '-W', '-f=${db:Status-Status}\n', *packages],
                            capture_output=True, text=True)
    return (status.returncode == 0 and status.stdout.splitlines() == ['installed'] * len(packages)
            and os.access(app.binary, os.X_OK))


def install(name: str, app: Application) -> None:
    asset_name = name + '-arm64.deb'
    checksums = shared.github_metadata(DOWNLOAD + 'SHA256SUMS').decode('ascii')
    pattern = re.compile(r'([0-9a-f]{64})  ' + re.escape(asset_name))
    matches = [match.group(1) for line in checksums.splitlines()
               if (match := pattern.fullmatch(line)) is not None]
    if len(matches) != 1:
        raise ValueError(f'Missing or ambiguous installer checksum: {asset_name}')
    installer = shared.download(DOWNLOAD + asset_name, matches[0], '.deb')
    fields = subprocess.check_output(['dpkg-deb', '-f', str(installer), 'Package', 'Architecture'], text=True)
    if fields.splitlines() != [f'Package: {app.package}', 'Architecture: arm64']:
        raise ValueError('Installer package or architecture does not match the application')
    env = dict(os.environ, DEBIAN_FRONTEND='noninteractive')
    subprocess.run(['sudo', 'apt-get', 'update'], check=True, env=env)
    subprocess.run(['sudo', 'apt-get', 'install', '-y', '--no-install-recommends', str(installer), *app.dependencies],
                   check=True, env=env)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('application', choices=APPS)
    parser.add_argument('arguments', nargs='*')
    args = parser.parse_args()
    if not shutil.which('apt-get') or subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip() != 'arm64':
        raise RuntimeError('These installers require an ARM64 Debian-family instance')
    app = APPS[args.application]
    cache = Path(os.environ.get('XDG_CACHE_HOME', Path.home()/'.cache'))/'arlinux/popular-apps'
    cache.mkdir(parents=True, exist_ok=True)
    print(f'Checking {args.application} installation…', flush=True)
    with shared.locked(cache/'install.lock'):
        if not installed(app):
            print(f'Installing {args.application}…', flush=True)
            install(args.application, app)
            if not installed(app):
                raise RuntimeError('Application installation did not complete')
    # Keep the on-demand entry as the single Apps entry after APT installs the
    # vendor shortcut. Preserve any existing user override.
    applications = Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'applications'
    applications.mkdir(parents=True, exist_ok=True)
    try:
        with (applications/app.desktop).open('x') as entry:
            entry.write('[Desktop Entry]\nType=Application\nHidden=true\n')
    except FileExistsError:
        pass
    env = desktop_bus.environment(os.environ)
    env.update(app.environment)
    with (cache/(args.application + '.log')).open('ab') as log:
        subprocess.Popen([app.binary, *app.flags, *args.arguments], env=env,
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
