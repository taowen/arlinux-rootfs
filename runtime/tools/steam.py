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
import select
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
import shared

CDN = 'https://client-update.fastly.steamstatic.com/'
CLIENT_CDNS = (CDN, 'https://client-update.akamai.steamstatic.com/',
               'https://client-update.steamstatic.com/')
CHANNELS = ('stable', 'publicbeta')
PREINSTALLED = Path('/opt/arlinux/steam-client')
CLIENT_MANIFEST = 'steam_client_linuxarm64.manifest'
LEGACY_MANIFEST = 'arlinux-stable.manifest'
INSTALLED_MANIFEST = 'steam_client_linuxarm64.installed'


def normalize_client_manifests(root):
    """Migrate the old channel marker to Valve's pending-manifest name."""
    package = root/'package'
    legacy = package/LEGACY_MANIFEST
    current = package/CLIENT_MANIFEST
    if legacy.is_file():
        package.mkdir(parents=True, exist_ok=True)
        if current.exists() or current.is_symlink():
            legacy.unlink()
        else:
            legacy.replace(current)


def client_bootstrap_ready(root):
    return (root/'steamrtarm64/steam').is_file()


def client_install_recorded(root):
    return (root/'package'/INSTALLED_MANIFEST).is_file()


def share_client(root, previous):
    """Move one existing client without copying depots or merging accounts."""
    if (root/'steamrtarm64/steam').is_file():
        return
    if not (previous/'steamrtarm64/steam').is_file():
        return
    if client_processes(previous):
        raise RuntimeError('Close the existing Steam client before moving it to shared storage')
    if root.exists():
        if any(root.iterdir()):
            raise RuntimeError('Preserving an incomplete shared Steam installation; use --update to repair it')
        root.rmdir()
    previous.rename(root)
    # Keep existing SDK links and stored game-library paths valid through the
    # move. The alias belongs to this instance; deleting it cannot delete Steam.
    previous.symlink_to(root, target_is_directory=True)
    listing = root/'steamapps/libraryfolders.vdf'
    if listing.is_file():
        old = json.dumps(str(previous))
        new = json.dumps(str(root))
        text = listing.read_text()
        updated = re.sub(r'("path"\s+)' + re.escape(old), lambda m: m[1] + new, text)
        if updated != text:
            temporary = listing.with_suffix('.tmp')
            temporary.write_text(updated)
            temporary.replace(listing)
    print('Moved Steam and its downloaded games/runtimes into /arlinux-shared/steam.', flush=True)


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
    sources = client_sources(url)
    for index, source in enumerate(sources):
        try:
            subprocess.run(['curl', '--fail', '--location', '--silent', '--show-error', '--retry', '1',
                            '--connect-timeout', '5', '--max-time', '30', '--speed-limit', '1024',
                            '--speed-time', '10', '--output', str(destination), source], check=True)
            return
        except subprocess.CalledProcessError:
            if index == len(sources) - 1:
                raise


def client_sources(url):
    if url.startswith(CDN):
        return [base + url[len(CDN):] for base in CLIENT_CDNS]
    return [url]


def dependencies():
    """Use the distribution package manager, never a private library bundle."""
    if shutil.which('apt-get'):
        subprocess.run(['sudo', 'apt-get', 'update'], check=True)
        packages = ['ca-certificates', 'curl', 'xdg-user-dirs', 'libgtk2.0-0t64',
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
                        'ca-certificates', 'curl', 'xdg-user-dirs', 'pipewire', 'libnm',
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
    missing += [name for name in ('curl', 'lsof', 'xdg-user-dir') if not shutil.which(name)]
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


def links(root, previous=None):
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
        if (previous is not None and previous != root and path.is_symlink()
                and path.resolve() == (previous/target.relative_to(root)).resolve()):
            # Only aliases owned by this launcher's old installation. Keep the
            # old client itself untouched when a shared installation exists.
            path.unlink()
            path.symlink_to(target)
        if name in old_targets and path.is_symlink() and path.resolve() == old_targets[name]:
            path.unlink()
        if path.is_symlink() or path.exists():
            if path.resolve() != target:
                raise RuntimeError(f'Preserving another Steam installation: {path}')
        else:
            path.symlink_to(target)


def install(root, channel):
    if client_processes(root):
        raise RuntimeError('Close Steam before installing or repairing it')
    # Check existing installation ownership before downloading or changing files.
    links(root)
    dependencies()
    cache = shared.directory('steam')
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
            print(f'Steam build {version}: {name} ({index}/{len(parts)})', flush=True)
            package = shared.download(client_sources(CDN + filename), expected, '.zip')
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
        # Keep a pending channel listing under Valve's own name. Do not write
        # an arlinux-* manifest: the ARM64 bootstrap can mistake it for its
        # installed-file record and skip the real client download.
        if channel == 'stable':
            shutil.copyfile(listing, root/f'package/{CLIENT_MANIFEST}')
    # Installation/repair can already have a config (for example restored
    # authentication). Set only these defaults here, never on ordinary startup.
    config = root/'config/config.vdf'
    config.parent.mkdir(parents=True, exist_ok=True)
    if config.is_symlink():
        raise RuntimeError('Refusing to replace a redirected Steam configuration')
    original = config.read_text() if config.exists() else ''
    settings = keyvalues(original)
    system = settings
    for name in ('InstallConfigStore', 'Software', 'Valve', 'Steam', 'System'):
        key = next((key for key in system if key.lower() == name.lower()), name)
        system = system.setdefault(key, {})
        if not isinstance(system, dict):
            raise ValueError('Invalid Steam configuration block: '+name)
    for name in ('IdleScreensaverBatterySeconds', 'IdleScreensaverACSeconds'):
        key = next((key for key in system if key.lower() == name.lower()), name)
        system[key] = '0'

    def encode(values, indent=''):
        lines = []
        for key, value in values.items():
            if isinstance(value, dict):
                lines.extend([indent+json.dumps(key), indent+'{', encode(value, indent+'\t'), indent+'}'])
            else:
                lines.append(indent+json.dumps(key)+'\t\t'+json.dumps(value))
        return '\n'.join(lines)

    with tempfile.NamedTemporaryFile(mode='w', dir=config.parent, delete=False) as output:
        temporary_config = Path(output.name)
    try:
        temporary_config.write_text(encode(settings)+'\n')
        temporary_config.chmod(config.stat().st_mode & 0o777 if config.exists() else 0o600)
        if client_processes(root) or (config.read_text() if config.exists() else '') != original:
            raise RuntimeError('Steam configuration changed during installation; retry with Steam closed')
        temporary_config.replace(config)
    finally:
        temporary_config.unlink(missing_ok=True)
    print(f'Steam ARM64 bootstrap build {version} ready; starting Valve updater.', flush=True)


def runtime_installed(root):
    from steam_fex import app_directory, graphics_runtime, IncompleteRuntime
    try:
        app_directory(root, '3127680')
        graphics_runtime(root)
        return True
    except IncompleteRuntime:
        return False




def client_running():
    """Like Valve's launcher, require a live PID holding Steam's IPC FIFO.

    A leftover PID file alone is not evidence of a running client.
    """
    steam = Path.home()/'.steam'
    try:
        pid = int((steam/'steam.pid').read_text().strip())
        if pid <= 0 or not stat.S_ISFIFO((steam/'steam.pipe').stat().st_mode):
            return False
        for fd in Path(f'/proc/{pid}/fd').iterdir():
            try:
                if fd.samefile(steam/'steam.pipe'):
                    return True
            except FileNotFoundError:
                pass  # Descriptors can close while enumerating them.
    except (FileNotFoundError, ValueError):
        return False
    return False


def client_processes(root):
    """Find live client executables, excluding helpers and stale zombies."""
    binary = os.fsencode(str(root/'steamrtarm64/steam'))
    result = []
    for process in Path('/proc').glob('[0-9]*'):
        try:
            if process.stat().st_uid != os.getuid():
                continue
            status = (process/'status').read_text()
            if re.search(r'^State:\s+Z', status, re.MULTILINE):
                continue
            argv = (process/'cmdline').read_bytes().split(b'\0')
            if argv[0] == binary:
                result.append(int(process.name))
        except (OSError, ValueError):
            continue  # A process can exit while inspecting it.
    return result


def send_client_command(root, arguments):
    # Valve's ARM64 client downloads Steam Runtime 4 itself. Command forwarding
    # must work before the optional FEX/Sniper depots have been installed.
    candidates = sorted((root/'steamapps/common').glob(
        'SteamLinuxRuntime_*/pressure-vessel-arm64/bin/steam-runtime-steam-remote'))
    for remote in candidates:
        try:
            with remote.open('rb') as executable:
                header = executable.read(20)
            if header[:5] != b'\x7fELF\x02' or header[5] != 1 or int.from_bytes(header[18:20], 'little') != 183:
                continue
        except OSError:
            continue
        subprocess.run([str(remote), *arguments], check=True, timeout=30)
        return
    # Steam's public remote helper uses a shell-quoted command line in this
    # FIFO. Use the same protocol when no optional runtime depot exists yet.
    # Reference: steam-runtime-tools, _srt_steam_command_via_pipe (utils.c).
    if not client_running():
        raise RuntimeError('Steam is no longer running')
    if any(any(character in argument for character in ('\0', '\n', '\r')) for argument in arguments):
        raise ValueError('Invalid Steam command argument')
    payload = (shlex.join(['~/.steam/root/ubuntu12_32/steam', *arguments]) + '\n').encode()
    fd = os.open(Path.home()/'.steam/steam.pipe', os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOCTTY)
    try:
        if not stat.S_ISFIFO(os.fstat(fd).st_mode):
            raise RuntimeError('Steam command channel is not a FIFO')
        if len(payload) > os.fpathconf(fd, 'PC_PIPE_BUF'):
            raise ValueError('Steam command exceeds the atomic pipe limit')
        if not select.select([], [fd], [], 10)[1]:
            raise RuntimeError('Steam command service did not respond')
        if os.write(fd, payload) != len(payload):
            raise RuntimeError('Steam command was incomplete')
    finally:
        os.close(fd)


def system_language_arguments(arguments):
    """Apply Android's current language on every cold start, including restarts."""
    chinese = shared.acceleration_enabled()
    filtered = []
    values = iter(arguments)
    for value in values:
        if value == '-language':
            next(values, None)
        elif not value.startswith('-language='):
            filtered.append(value)
    return ['-language', 'schinese' if chinese else 'english', *filtered]


def launch(root, arguments):
    """Prepare once, then replace this process with Valve; Android owns side services."""
    binary = root/'steamrtarm64/steam'
    if not binary.is_file():
        raise RuntimeError('Steam installation has no native ARM64 executable')
    if client_running():
        send_client_command(root, arguments or ['steam://open/main'])
        return 0
    arguments = system_language_arguments(arguments)
    arguments = ['-noipv6', *[arg for arg in arguments if arg != '-noipv6']]
    # The ARM64 client requires CefCrashReportingEnabled(), but its package does
    # not supply CEF's configuration. Enable local crash reports using CEF's
    # supported file; omitting ServerURL keeps reports on the device. Preserve
    # any vendor/user configuration instead of suppressing Valve assertions.
    try:
        with (binary.parent/'crash_reporter.cfg').open('x') as config:
            config.write('[Config]\nProductName=Steam\n')
    except FileExistsError:
        pass
    ensure_dependencies()
    if Path('/etc/ld.so.conf.d/00-arlinux-graphics.conf').is_file():
        subprocess.run(['sudo', 'ldconfig'], check=True)
    links(root)
    from steam_fex import configure
    from steam_bionic import configure as configure_bionic
    from steam_proxy import prepare, activate, ADDRESS
    configure_bionic(root)
    ready = runtime_installed(root)
    if ready:
        configure(root)
    relay = prepare()
    import socket
    request = json.dumps({'home': str(Path.home()), 'arguments': arguments,
                          'preparingPid': os.getpid(),
                          'prepareRuntime': not ready, 'relay': relay,
                          'address': ADDRESS}).encode() + b'\n'
    # The host service is global to this Android package and guest lifecycle.
    package = Path(os.environ['BIONICX_FILES']).parent.name
    with socket.socket(socket.AF_UNIX) as control:
        control.settimeout(30)
        control.connect('\0' + package + '.steam-session')
        control.sendall(request)
        response = bytearray()
        while not response.endswith(b'\n'):
            block = control.recv(4096)
            if not block or len(response) > 65536:
                raise RuntimeError('Steam host service did not acknowledge preparation')
            response.extend(block)
        acknowledgement = json.loads(response)
        if not acknowledgement['ok']:
            raise RuntimeError('Steam host service could not prepare the session')
    if relay and acknowledgement.get('relay', False):
        try:
            endpoint = activate()
            arguments.append("-cef-http-proxy=" + endpoint)
        except Exception:
            print('Steam HTTPS acceleration unavailable; using direct connections.', flush=True)
    elif relay:
        print('Steam HTTPS acceleration unavailable; using direct connections.', flush=True)
    # Prepare the Windows compatibility runtime during Steam installation/startup,
    # before handing control to Valve. Play must not fetch Wine for the first time.
    import windows
    print('Preparing Steam Windows compatibility runtime…', flush=True)
    windows.ensure_runtime()
    print('Steam Windows compatibility runtime ready.', flush=True)
    os.chdir(root)
    # CLOEXEC releases startup serialization atomically with Valve's exec.
    os.execv(str(binary), [str(binary), *arguments])



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
    previous = (Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'Steam').resolve()
    root = shared.ROOT/'steam'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--command', action='store_true')
    parser.add_argument('--desktop', action='store_true',
                        help='Keep the launcher terminal open if startup fails')
    parser.add_argument('--update', action='store_true', help='Refresh the native bootstrap, then run Valve updater')
    parser.add_argument('--channel', choices=CHANNELS, help='Select a Valve ARM64 channel (default: stable)')
    args, steam_arguments = parser.parse_known_args()
    require_robust_futex()
    if args.command:
        send_client_command(root, steam_arguments)
        return 0
    # Serialize preparation only. Keep Python's close-on-exec flag: inheriting
    # this lock into Valve's game wrappers lets an orphaned game block later
    # Steam starts even after the real client has exited. Client process/FIFO
    # checks below handle the running-client phase.
    session = (shared.directory('locks')/'steam-session.lock').open('a')
    try:
        fcntl.flock(session, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        if client_running():
            send_client_command(root, steam_arguments or ['steam://open/main'])
            return 0
        raise RuntimeError('Steam is starting in another session. Wait for its window to open.')
    if client_running():
        if args.update or args.channel:
            raise RuntimeError('Close Steam before updating or changing its channel')
        if previous != root and (previous/'steamrtarm64/steam').is_file():
            raise RuntimeError('Close the instance-local Steam client before using shared Steam')
        send_client_command(root, steam_arguments or ['steam://open/main'])
        return 0
    if client_processes(root):
        raise RuntimeError('Shared Steam is still running in another session. Close it first.')
    if previous != root:
        share_client(root, previous)
    normalize_client_manifests(root)
    links(root, previous)
    if not args.update and not args.channel:
        seed_client(root)
        normalize_client_manifests(root)
    # A bootstrap executable alone is not a completed installation.
    # Re-download it when no Valve install record or pending manifest is
    # present; otherwise let Valve's updater finish missing depots.
    pending = (root/'package'/CLIENT_MANIFEST).is_file()
    if (args.update or args.channel or not client_bootstrap_ready(root)
            or (not client_install_recorded(root) and not pending)):
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
