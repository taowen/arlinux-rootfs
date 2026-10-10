#!/usr/bin/env python3
"""Install Valve's native ARM64 client; launch it without graphics/sandbox overrides."""
import argparse
import ctypes.util
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

CDN = 'https://client-update.fastly.steamstatic.com/'
CHANNELS = ('stable', 'publicbeta')
PREINSTALLED = Path('/opt/arlinux/steam-client')


def seed_client(root):
    """Populate a new user's installation from the optional, account-free seed.

    Each instance has its own extracted seed. Move it into the user's directory
    instead of copying gigabytes; Valve updates it normally thereafter. Existing
    installations and explicitly requested channels are never replaced.
    """
    if (root/'steamrtarm64/steam').is_file() or not (PREINSTALLED/'steamrtarm64/steam').is_file():
        return
    if root.exists() and any(root.iterdir()):
        raise RuntimeError('Preserving an incomplete Steam installation; use --update to repair it')
    root.parent.mkdir(parents=True, exist_ok=True)
    if root.exists():
        root.rmdir()
    PREINSTALLED.rename(root)
    print('Using the preinstalled Steam client. Valve may check for updates.', flush=True)


def keyvalues(text):
    """Read Valve's quoted KeyValues, including nested blocks."""
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|[{}]', text)
    position = 0

    def block(nested=False):
        nonlocal position
        result = {}
        while position < len(tokens):
            key = tokens[position]
            position += 1
            if key == '}':
                if not nested:
                    raise ValueError('Unexpected manifest closing brace')
                return result
            if not key.startswith('"') or position == len(tokens):
                raise ValueError('Invalid manifest key')
            value = tokens[position]
            position += 1
            result[json.loads(key, strict=False)] = block(True) if value == '{' else json.loads(value, strict=False)
        if nested:
            raise ValueError('Unclosed manifest block')
        return result

    return block()


def manifest(text):
    return keyvalues(text)['linuxarm64']


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def fetch(url, destination):
    subprocess.run(['curl', '--fail', '--location', '--silent', '--show-error', '--retry', '3',
                    '--connect-timeout', '20', '--speed-limit', '1024',
                    '--speed-time', '60', '--output', str(destination), url], check=True)


def dependencies():
    """Use the distribution package manager, never a private library bundle."""
    if shutil.which('apt-get'):
        subprocess.run(['sudo', 'apt-get', 'update'], check=True)
        packages = ['ca-certificates', 'curl', 'libgtk2.0-0t64',
                    'libpipewire-0.3-0t64', 'libnm0', 'lsof', 'libnss3',
                    'libxss1', 'libasound2t64', 'libpulse0', 'libsdl2-2.0-0', 'libopenal1']
        subprocess.run(['sudo', 'apt-get', 'install', '-y', '--no-install-recommends',
                        *packages], check=True)
    elif shutil.which('pacman'):
        # GTK2 has left Arch's main repositories. Do not request a nonexistent
        # package or silently substitute GTK3 (its ABI is incompatible).
        if not ctypes.util.find_library('gtk-x11-2.0'):
            raise RuntimeError('This Arch desktop needs GTK2 installed before Steam; '
                               'automatic dependency setup is currently tested on Debian/Yibu/LXQt')
        subprocess.run(['sudo', 'pacman', '-S', '--needed', '--noconfirm',
                        'ca-certificates', 'curl', 'pipewire', 'libnm',
                        'lsof', 'nss', 'libxss', 'alsa-lib', 'libpulse', 'sdl2-compat', 'openal'], check=True)
    else:
        raise RuntimeError('Install Steam dependencies with your package manager: '
                           'GTK2, PipeWire, libnm, lsof, NSS, Xss, ALSA and PulseAudio')


def ensure_dependencies():
    """Repair missing host dependencies even for an existing installation."""
    libraries = ('libgtk-x11-2.0.so.0', 'libpipewire-0.3.so.0', 'libnm.so.0',
                 'libnss3.so', 'libXss.so.1', 'libasound.so.2', 'libpulse.so.0',
                 'libSDL2-2.0.so.0', 'libopenal.so.1')
    missing = []
    for name in libraries:
        try:
            ctypes.CDLL(name)
        except OSError:
            missing.append(name)
    missing += [name for name in ('curl', 'lsof') if not shutil.which(name)]
    if missing:
        print('Installing Steam dependencies: ' + ', '.join(missing), flush=True)
        dependencies()


def extract(archive, directory):
    """Stage checked components; reject paths and links escaping the installation."""
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            name = item.filename.replace('\\', '/')
            path = directory / name
            if name.startswith('/') or not path.resolve().is_relative_to(directory):
                raise RuntimeError(f'Unsafe Steam archive path: {name}')
            if item.is_dir():
                path.mkdir(parents=True, exist_ok=True)
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            mode = item.external_attr >> 16
            if stat.S_ISLNK(mode):
                target = source.read(item).decode()
                if Path(target).is_absolute() or not (path.parent / target).resolve().is_relative_to(directory):
                    raise RuntimeError(f'Unsafe Steam archive link: {name}')
                if path.is_symlink() or path.exists():
                    path.unlink()
                path.symlink_to(target)
            else:
                if path.is_symlink():
                    path.unlink()
                with source.open(item) as data, path.open('wb') as output:
                    shutil.copyfileobj(data, output)
                with path.open('rb') as data:
                    executable = data.read(4).startswith((b'\x7fELF', b'#!'))
                path.chmod(0o755 if executable or mode & 0o111 else 0o644)


def links(root):
    steam = Path.home()/'.steam'
    steam.mkdir(exist_ok=True)
    # Steam uses separate SDK directories for x86-64 and ARM64 clients. Games
    # load ~/.steam/sdk64/steamclient.so even when Steam itself runs on ARM64.
    targets = {'root': root, 'steam': root, 'bin64': root/'steamrtarm64',
                         'binarm64': root/'steamrtarm64', 'sdk64': root/'linux64',
                         'sdkarm64': root/'linuxarm64'}
    old_targets = {'sdk64': root/'linuxarm64'}
    for name, target in targets.items():
        path = steam/name
        if name in old_targets and path.is_symlink() and path.resolve() == old_targets[name]:
            path.unlink()
        if path.is_symlink() or path.exists():
            if path.resolve() != target:
                raise RuntimeError(f'Preserving another Steam installation: {path}')
        else:
            path.symlink_to(target)


def install(root, channel):
    if subprocess.run(['pgrep', '-f', str(root/'steamrtarm64/steam')],
                      stdout=subprocess.DEVNULL).returncode == 0:
        raise RuntimeError('Close Steam before installing or repairing it')
    # Check existing installation ownership before downloading or changing files.
    links(root)
    dependencies()
    cache = Path(os.environ.get('XDG_CACHE_HOME', Path.home()/'.cache'))/'arlinux/steam'
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='install-', dir=cache) as temporary:
        work = Path(temporary)
        listing = work/'manifest'
        manifest_name = 'steam_client_linuxarm64' if channel == 'stable' else f'steam_client_{channel}_linuxarm64'
        fetch(CDN + manifest_name, listing)
        info = manifest(listing.read_text())
        version = info['version']
        if not re.fullmatch(r'\d+', version):
            raise RuntimeError('Invalid Steam build number')
        component = info.get('bins_linuxarm64_linuxarm64')
        if not isinstance(component, dict):
            raise RuntimeError('Manifest has no native ARM64 client')
        parts = [('bins_linuxarm64_linuxarm64', component)]
        stage = work/'client'
        stage.mkdir()
        for index, (name, component) in enumerate(parts, 1):
            filename, expected = component['file'], component['sha2']
            if not re.fullmatch(r'[A-Za-z0-9_.-]+', filename) or not re.fullmatch(r'[0-9a-f]{64}', expected):
                raise RuntimeError('Invalid Steam component name or checksum')
            package = cache/filename
            print(f'Steam build {version}: {name} ({index}/{len(parts)})', flush=True)
            if not package.is_file() or digest(package) != expected:
                partial = work/filename
                fetch(CDN + filename, partial)
                if digest(partial) != expected:
                    raise RuntimeError(f'Steam SHA-256 verification failed: {name}')
                partial.replace(package)
            extract(package, stage)
        binary = stage/'steamrtarm64/steam'
        with binary.open('rb') as source:
            header = source.read(20)
        if header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 183:
            raise RuntimeError('Steam client is not an ARM64 Linux executable')
        root.mkdir(parents=True, exist_ok=True)
        # Reject existing symlinks which could redirect writes outside Steam.
        for path in stage.rglob('*'):
            destination = root/path.relative_to(stage)
            if not destination.resolve().is_relative_to(root):
                raise RuntimeError(f'Existing Steam path escapes installation: {destination}')
        for path in sorted(stage.rglob('*'), key=lambda p: len(p.parts)):
            destination = root/path.relative_to(stage)
            if path.is_symlink():
                if destination.exists() or destination.is_symlink():
                    destination.unlink()
                destination.symlink_to(os.readlink(path))
            elif path.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
            else:
                with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
                    temporary_file = Path(output.name)
                try:
                    shutil.copy2(path, temporary_file)
                    temporary_file.replace(destination)
                finally:
                    temporary_file.unlink(missing_ok=True)
        (root/'package').mkdir(exist_ok=True)
        if channel == 'stable':
            (root/'package/beta').unlink(missing_ok=True)
        else:
            (root/'package/beta').write_text(channel + '\n')
        # Only Valve's updater writes its installed-file manifest. An update
        # channel listing is not an installation record.
        shutil.copyfile(listing, root/f'package/arlinux-{channel}.manifest')
    print(f'Steam ARM64 bootstrap build {version} ready; starting Valve updater.', flush=True)


def runtime_installed(root):
    from steam_fex import app_directory, IncompleteRuntime
    try:
        app_directory(root, '3127680')
        runtime = app_directory(root, '1628350')
        return any(runtime.glob('sniper_platform_*/usr-mtree.txt.gz'))
    except IncompleteRuntime:
        return False


def bootstrap_runtime(binary, root, arguments):
    """Let Steam download its depots, then restart with the prepared provider.

    Environment changes cannot reach an already running client. This is a
    first-install session only; ordinary game launches remain owned by Steam.
    Never force-kill a client that declines its normal shutdown request.
    """
    print('First-time game setup: install FEX-Emu and Steam Linux Runtime 3.0 '
          '(sniper) in Steam. Steam will restart once both downloads finish.', flush=True)
    with subprocess.Popen([str(binary), *arguments]) as client:
        while True:
            try:
                return client.wait(timeout=5)
            except subprocess.TimeoutExpired:
                if not runtime_installed(root):
                    continue
            print('Game runtimes downloaded. Restarting Steam to prepare graphics.', flush=True)
            subprocess.run([str(binary), '-shutdown'], check=True, timeout=30)
            try:
                result = client.wait(timeout=60)
            except subprocess.TimeoutExpired:
                print('Steam is still busy. Close it normally to finish game setup.', flush=True)
                return client.wait()
            return 42 if result == 0 else result


def launch(root, arguments):
    binary = root/'steamrtarm64/steam'
    if not binary.is_file():
        raise RuntimeError('Steam installation has no native ARM64 executable')
    ensure_dependencies()
    # GPU overlays are extracted after the rootfs is built. Refresh the native
    # cache so pressure-vessel can discover their complete ELF dependencies.
    if Path('/etc/ld.so.conf.d/00-arlinux-graphics.conf').is_file():
        subprocess.run(['sudo', 'ldconfig'], check=True)
    links(root)
    from steam_fex import configure
    os.chdir(root)
    while True:
        if runtime_installed(root):
            configure(root)
            status = subprocess.run([str(binary), *arguments]).returncode
        else:
            status = bootstrap_runtime(binary, root, arguments)
        if status != 42:  # Valve updater or first-time runtime setup restart.
            if status < 0:
                print(f'Steam stopped with signal {-status}. '
                      f'Client logs: {root / "logs"}', file=sys.stderr)
                return 128 - status
            return status


def require_robust_futex():
    """Check the calling process, not an ADB process with a different seccomp policy."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    head, size = ctypes.c_void_p(), ctypes.c_size_t()
    # ARM64 Linux get_robust_list; querying does not change pthread's registration.
    result = libc.syscall(ctypes.c_long(100), ctypes.c_long(0),
                          ctypes.byref(head), ctypes.byref(size))
    if result < 0:
        error = ctypes.get_errno()
        reason = ('Android blocked the robust-futex system call in this app process'
                  if error in (errno.ENOSYS, errno.EPERM) else os.strerror(error))
        raise RuntimeError(f'{reason}. Steam requires it for inter-process locks. '
                           'This needs an ARLinux runtime fix, not a Steam reinstall.')
    if not head.value or size.value != 3 * ctypes.sizeof(ctypes.c_void_p):
        raise RuntimeError('The runtime has no valid pthread robust-futex registration.')


def main():
    if platform.machine() != 'aarch64':
        raise RuntimeError('Native Steam requires ARM64 Linux')
    root = (Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'Steam').resolve()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--desktop', action='store_true',
                        help='Keep the launcher terminal open if startup fails')
    parser.add_argument('--update', action='store_true', help='Refresh the native bootstrap, then run Valve updater')
    parser.add_argument('--channel', choices=CHANNELS, help='Select a Valve ARM64 channel (default: stable)')
    args, steam_arguments = parser.parse_known_args()
    require_robust_futex()
    cache = Path(os.environ.get('XDG_CACHE_HOME', Path.home()/'.cache'))/'arlinux/steam'
    cache.mkdir(parents=True, exist_ok=True)
    with (cache/'install.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not args.update and not args.channel:
            seed_client(root)
        if args.update or args.channel or not (root/'steamrtarm64/steam').is_file():
            install(root, args.channel or CHANNELS[0])
    return launch(root, steam_arguments)


if __name__ == '__main__':
    try:
        status = main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError,
            zipfile.BadZipFile, KeyError) as error:
        print(f'Steam: {error}', file=sys.stderr)
        status = 1
    if status and '--desktop' in sys.argv and sys.stdin.isatty():
        try:
            input('\nSteam did not start. Press Enter to close this window. ')
        except (EOFError, KeyboardInterrupt):
            pass
    sys.exit(status)
