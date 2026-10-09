#!/usr/bin/env python3
"""Install the pinned official OpenCode Desktop on first use, then launch it."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import subprocess
import sys

BINARY = Path('/opt/OpenCode/ai.opencode.desktop')
MANIFEST = Path('/usr/lib/arlinux/guest/opencode-downloads.tsv')


def install(cache):
    row = next(line.split('\t') for line in MANIFEST.read_text().splitlines()
               if line.startswith('opencode-desktop\t'))
    _, version, expected, url = row
    extension = url.rsplit('.', 1)[-1]
    if (len(expected) != 64 or any(c not in '0123456789abcdef' for c in expected)
            or not url.startswith('https://') or extension not in ('deb', 'rpm')):
        raise ValueError('Invalid OpenCode download manifest')
    print(f'Preparing OpenCode {version}. The first download may take a few minutes.\n'
          'Internet access is required. Subsequent launches use the installed app.', flush=True)
    package = cache / f'{expected}.{extension}'

    def verified():
        if not package.is_file():
            return False
        with package.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest() == expected

    if not verified():
        partial = package.with_suffix(package.suffix + '.part')
        try:
            subprocess.run(['curl', '-fL', '--retry', '3', '--connect-timeout', '20',
                            '-o', str(partial), url], check=True)
            with partial.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != expected:
                    raise ValueError('OpenCode download checksum mismatch')
            partial.replace(package)
        finally:
            partial.unlink(missing_ok=True)
    env = dict(os.environ, DEBIAN_FRONTEND='noninteractive')
    if extension == 'deb':
        subprocess.run(['sudo', 'apt-get', 'update'], env=env, check=True)
        subprocess.run(['sudo', 'apt-get', 'install', '-y', '--no-install-recommends',
                        str(package)], env=env, check=True)
    else:
        # The official ARM64 RPM contains an ordinary Linux application payload.
        subprocess.run(['sudo', 'pacman', '-S', '--needed', '--noconfirm',
                        'libarchive', 'gtk3', 'nss', 'libxss', 'libxtst', 'libsecret',
                        'alsa-plugins', 'libpulse', 'cups', 'libdrm', 'mesa'], check=True)
        subprocess.run(['sudo', 'bsdtar', '-xpf', str(package), '-C', '/'], check=True)
    if not os.access(BINARY, os.X_OK):
        raise RuntimeError('OpenCode executable missing after installation')
    # Keep one Apps entry: the package ships two vendor launchers. Standard
    # per-user desktop overrides hide them without modifying package files.
    applications = Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'applications'
    applications.mkdir(parents=True, exist_ok=True)
    for name in ('ai.opencode.desktop.desktop', 'opencode-desktop.desktop'):
        (applications / name).write_text('[Desktop Entry]\nType=Application\nName=OpenCode\nHidden=true\n')
    package.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--desktop', action='store_true', help='Keep errors visible in the launcher terminal')
    parser.parse_args()
    cache = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'arlinux/opencode'
    cache.mkdir(parents=True, exist_ok=True)
    with (cache / 'install.lock').open('w') as lock:
        if not os.access(BINARY, os.X_OK):
            print('Checking OpenCode installation…', flush=True)
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not os.access(BINARY, os.X_OK):
            install(cache)
    # Keep the same accessibility and rendering path for Apps and voice AI.
    # Once preparation finishes, close its terminal without closing the GUI.
    with (cache / 'desktop.log').open('ab') as log:
        subprocess.Popen([str(BINARY), '--force-renderer-accessibility',
                          '--ozone-platform=x11'], cwd='/', start_new_session=True,
                         stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
    return 0


if __name__ == '__main__':
    try:
        status = main()
    except (OSError, ValueError, RuntimeError, StopIteration, subprocess.CalledProcessError) as error:
        print(f'OpenCode: {error}\nCheck your connection and click OpenCode to retry.', file=sys.stderr)
        status = 1
    if status and '--desktop' in sys.argv and sys.stdin.isatty():
        try:
            input('\nPress Enter to close this window. ')
        except (EOFError, KeyboardInterrupt):
            pass
    sys.exit(status)
