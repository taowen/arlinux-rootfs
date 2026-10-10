"""Install an ARLinux Proton bundle and run a Windows executable.

Proton owns Wine/FEX setup. Each executable gets a private prefix; Steam's
installation, compatibility-tool selection and game prefixes are untouched.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile


MANIFEST = Path('/usr/lib/arlinux/guest/windows-downloads.tsv')


def ensure_runtime():
    """Download the pinned ARLinux GE release only when no runtime is installed."""
    if (storage()/'current.json').is_file():
        return runtime()
    fields = MANIFEST.read_text().strip().split('\t')
    if len(fields) != 3:
        raise ValueError('Invalid Windows runtime download manifest')
    version, expected, url = fields
    if (len(expected) != 64 or any(c not in '0123456789abcdef' for c in expected)
            or not url.startswith('https://github.com/taowen/proton-ge-custom/releases/download/')):
        raise ValueError('Invalid Windows runtime download source or checksum')
    cache = Path(os.environ.get('XDG_CACHE_HOME', Path.home()/'.cache'))/'arlinux/windows'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache/(expected+'.tar.gz')
    with (cache/'download.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not archive.is_file() or digest(archive) != expected:
            if not shutil.which('curl'):
                raise RuntimeError('Install curl using the distribution package manager first')
            print(f'Downloading {version}. The first installation is large and needs internet access.', flush=True)
            partial = archive.with_suffix('.part')
            try:
                subprocess.run(['curl', '-fL', '--retry', '3', '--connect-timeout', '20',
                                '--speed-limit', '1024', '--speed-time', '60',
                                '-o', str(partial), url], check=True)
                if digest(partial) != expected:
                    raise ValueError('Downloaded Windows runtime SHA-256 mismatch')
                partial.replace(archive)
            finally:
                partial.unlink(missing_ok=True)
        install(archive, expected)
        # The extracted runtime is already installed and verified; don't keep a
        # second 650 MiB compressed copy in the user's instance indefinitely.
        archive.unlink()
    return runtime()


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def validate(root):
    manifest = json.loads((root/'arlinux-runtime.json').read_text())
    if manifest.get('format') != 1 or manifest.get('architecture') != 'aarch64':
        raise ValueError('Unsupported Proton bundle format or architecture')
    required = ('proton', 'files/bin-arm64/wine', 'files/bin-arm64/wineserver',
                'files/lib/wine/aarch64-unix/ntdll.so')
    files = manifest.get('files', {})
    if not isinstance(files, dict) or not all(name in files for name in required):
        raise ValueError('Incomplete Proton bundle manifest')
    for name, expected in files.items():
        if not isinstance(name, str) or not isinstance(expected, str) or len(expected) != 64:
            raise ValueError('Invalid Proton file checksum')
        path = root/name
        if not path.resolve().is_relative_to(root.resolve()):
            raise ValueError('Runtime file escapes the bundle')
        if digest(path) != expected:
            raise ValueError(f'Proton bundle checksum mismatch: {name}')
    for name in required[1:]:
        with (root/name).open('rb') as source:
            header = source.read(20)
        if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\xb7\x00':
            raise ValueError(f'Not a native ARM64 ELF: {name}')
    return manifest


def storage():
    return Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'arlinux/windows'


def install(archive, expected):
    """Install atomically; never overwrite binaries used by a running process."""
    actual = digest(archive)
    if actual != expected.lower():
        raise ValueError('Archive SHA-256 mismatch')
    home = storage()
    runtimes = home/'runtimes'
    runtimes.mkdir(parents=True, exist_ok=True)
    destination = runtimes/actual
    with (home/'install.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not destination.exists():
            print('Installing Windows runtime. The first installation may take a few minutes.', flush=True)
            with tempfile.TemporaryDirectory(prefix='install-', dir=runtimes) as temporary:
                stage = Path(temporary)/'runtime'
                stage.mkdir()
                with tarfile.open(archive, 'r:*') as bundle:
                    bundle.extractall(stage, filter='data')
                manifest = validate(stage)
                if manifest.get('development'):
                    print('This is an experimental Windows runtime.', flush=True)
                stage.rename(destination)
        else:
            validate(destination)
        pointer = home/'current.json'
        temporary = pointer.with_suffix('.tmp')
        temporary.write_text(json.dumps({'sha256': actual})+'\n')
        temporary.replace(pointer)
    print(f'Installed Windows runtime: {actual[:12]}', flush=True)


def runtime():
    home = storage()
    pointer = home/'current.json'
    if not pointer.is_file():
        raise RuntimeError('Install a runtime first: arlinux-windows --install BUNDLE --sha256 SHA256')
    identity = json.loads(pointer.read_text()).get('sha256', '')
    if not isinstance(identity, str) or len(identity) != 64 or any(c not in '0123456789abcdef' for c in identity):
        raise ValueError('Invalid runtime identity')
    root = home/'runtimes'/identity
    # The immutable install is checked once during installation. Do not hash
    # gigabytes of runtime files on every application startup.
    if not (root/'arlinux-runtime.json').is_file():
        raise RuntimeError('Installed Windows runtime is missing')
    return root


def launch(executable, arguments, prefix):
    executable = executable.resolve(strict=True)
    if not executable.is_file():
        raise ValueError('Expected a Windows executable file')
    root = ensure_runtime()
    home = storage()
    identity = hashlib.sha256(os.fsencode(executable)).hexdigest()[:24]
    compat = prefix.resolve() if prefix else home/'prefixes'/identity
    compat.mkdir(parents=True, exist_ok=True)
    client = home/'client'
    client.mkdir(exist_ok=True)
    env = os.environ.copy()
    for name in ('LD_PRELOAD', 'FEX_ROOTFS', 'FEX_ENV', 'FEX_APP_CONFIG',
                 'FEX_APP_CONFIG_LOCATION', 'WINEPREFIX', 'WINELOADER',
                 'WINESERVER', 'WINEDLLPATH', 'WINEARCH'):
        env.pop(name, None)
    env.update(STEAM_COMPAT_DATA_PATH=str(compat),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(client),
               SteamAppId='0', SteamGameId='0', PROTON_USE_XALIA='0')
    # Preserve the user's logging preferences, IME and graphics configuration.
    env.setdefault('WINEDEBUG', '-all')
    return subprocess.call([str(root/'proton'), 'run', str(executable), *arguments],
                           cwd=executable.parent, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install', type=Path, metavar='BUNDLE')
    parser.add_argument('--sha256', help='Published SHA-256 of the runtime archive')
    parser.add_argument('--prefix', type=Path, help='Optional shared Proton data directory')
    parser.add_argument('executable', nargs='?', type=Path)
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if platform.machine() not in ('aarch64', 'arm64'):
        parser.error('This runtime requires an ARM64 Linux environment')
    if args.install:
        if not args.sha256:
            parser.error('--install requires --sha256')
        install(args.install, args.sha256)
    elif args.sha256:
        parser.error('--sha256 requires --install')
    if args.executable:
        return launch(args.executable, args.arguments, args.prefix)
    if not args.install:
        parser.error('Provide an executable or --install BUNDLE --sha256 SHA256')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, tarfile.TarError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'arlinux-windows: {error}')
