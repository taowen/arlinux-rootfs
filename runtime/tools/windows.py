"""Install Bionic Wine and launch Windows programs through the Android host."""
import argparse
import hashlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re
import socket
import shutil
import subprocess
import tarfile
import tempfile
import time
import shared
import desktop_bus

MANIFEST = Path('/usr/lib/arlinux/guest/windows-downloads.tsv')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def storage():
    return Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'arlinux/windows-bionic'


def instance_prefix():
    """Return the single Wine prefix owned by this Linux instance.

    Steam games and Windows launcher apps deliberately share this prefix.  The
    game install roots remain separate, but all Wine state and ``C:\\Program
    Files`` contents live in this one instance-level tree.
    """
    prefix = storage()/'prefix'
    prefix.mkdir(parents=True, exist_ok=True)
    return prefix


def runtime_storage():
    return shared.directory('windows-bionic')


def runtime(identity=None):
    if identity is None:
        identity = (storage()/'runtime-id').read_text().strip()
    if not isinstance(identity, str) or not re.fullmatch('[0-9a-f]{64}', identity):
        raise ValueError('Invalid runtime identity')
    root = runtime_storage()/identity
    metadata = json.loads((root/'bionic-runtime.json').read_text())
    if metadata.get('protocol') != 1 or metadata.get('libc') != 'bionic':
        raise ValueError('Only Bionic Wine is supported')
    return root


def _installer():
    """Fetch the one Wine-owned bootstrap; component policy lives in that package."""
    import types
    import sys
    rows = [line.split('\t') for line in MANIFEST.read_text().splitlines() if line.strip() and not line.startswith('#')]
    if len(rows) != 1 or len(rows[0]) != 4:
        raise ValueError('Expected one Wine installer endpoint')
    name = 'wine-installer'
    state = storage()/'installer.json'
    selected = None
    overrides = os.environ.get('ARLINUX_WINE_COMPONENTS')
    override = Path(overrides)/(name+'.json') if overrides else None
    if override is not None and override.is_file():
        expected = json.loads(override.read_text())['sha256']
        source = shared.directory('downloads')/(expected+'.json')
        if not re.fullmatch('[0-9a-f]{64}', expected) or digest(source) != expected:
            raise ValueError('Wine installer descriptor checksum mismatch')
        selected = json.loads(source.read_text())
    else:
        try:
            cached = json.loads(state.read_text())
            if cached['endpoint'] == rows[0] and 0 <= time.time()-cached['checkedAt'] < 3600:
                selected = cached['asset']
        except (FileNotFoundError, ValueError, KeyError, TypeError):
            pass
        if selected is None:
            selected = _bootstrap_release_asset(rows[0])
        expected = selected['sha256']
    identity = selected['sha256'] if selected.get('name') == name else expected
    if not re.fullmatch('[0-9a-f]{64}', identity):
        raise ValueError('Invalid Wine installer identity')
    home = runtime_storage()/'components'/name
    home.mkdir(parents=True, exist_ok=True)
    root = home/identity
    with shared.locked(home/'install.lock'):
        if not root.is_dir():
            if selected.get('kind') == 'arlinux-bionic-components' and selected.get('format') == 1:
                archives, metadata = selected['components'], selected['runtime']
            elif selected.get('name') == name:
                archives, metadata = [selected], None
            else:
                raise ValueError('Invalid Wine installer descriptor')
            with tempfile.TemporaryDirectory(prefix='bootstrap-', dir=home) as temporary:
                stage = Path(temporary)/'installer'
                stage.mkdir()
                seen = set()
                for item in archives:
                    asset = item.get('asset', item['file'])
                    if (not re.fullmatch('[0-9a-f]{64}', item['sha256']) or not re.fullmatch(
                        r'https://github\.com/taowen/wine/releases/download/[^/]+/'+re.escape(asset), item['url'])):
                        raise ValueError('Invalid Wine installer archive')
                    archive = shared.download(item['url'], item['sha256'], '.tar.gz', progress=False)
                    if item.get('bytes') is not None and archive.stat().st_size != item['bytes']:
                        raise ValueError('Wine installer size mismatch')
                    with tarfile.open(archive, 'r:*') as bundle:
                        for member in bundle:
                            if member.name == 'bionic-runtime.json':
                                if metadata is not None or not member.isfile() or member.size > 4*1024*1024:
                                    raise ValueError('Invalid installer metadata')
                                metadata = json.load(bundle.extractfile(member))
                                continue
                            if (member.name in seen or '\' in member.name or ':' in member.name or
                                any(part in ('', '.', '..') for part in member.name.split('/')) or
                                not member.isfile()):
                                raise ValueError('Unsafe Wine installer archive path')
                            seen.add(member.name)
                            bundle.extract(member, stage, filter='data')
                if not metadata or metadata.get('installerProtocol') != 1:
                    raise ValueError('Unsupported Wine installer protocol')
                (stage/'bionic-runtime.json').write_text(json.dumps(metadata)+'\n')
                stage.rename(root)
        metadata = json.loads((root/'bionic-runtime.json').read_text())
        if metadata.get('installerProtocol') != 1:
            raise ValueError('Unsupported Wine installer protocol')
        for relative, checksum in metadata['files'].items():
            file = root/relative
            if (not file.resolve().is_relative_to(root.resolve()) or not re.fullmatch('[0-9a-f]{64}', checksum)
                or digest(file) != checksum):
                raise ValueError('Wine installer file checksum mismatch')
    if override is None or not override.is_file():
        state.parent.mkdir(parents=True, exist_ok=True)
        with shared.locked(state.with_suffix('.lock')):
            pending = state.with_suffix('.tmp')
            pending.write_text(json.dumps(dict(endpoint=rows[0], checkedAt=time.time(), asset=selected))+'\n')
            pending.replace(state)
    module_name = 'arlinux_wine_installer_'+identity
    if module_name not in sys.modules:
        source = root/'files/share/arlinux/wine-installer/runtime.py'
        module = types.ModuleType(module_name)
        module.__file__ = str(source)
        # Do not create bytecode files inside an immutable downloaded component.
        exec(compile(source.read_bytes(), str(source), 'exec'), module.__dict__)
        sys.modules[module_name] = module
    return sys.modules[module_name]


def ensure_runtime():
    return _installer().ensure_runtime()


def _bootstrap_release_asset(row):
    name, repository, tag, filename = row
    if row != ['wine-installer', 'taowen/wine', 'arlinux-wine-installer-latest', 'runtime.tar.gz']:
        raise ValueError('Invalid Wine installer endpoint')
    endpoint = f'https://api.github.com/repos/{repository}/releases/tags/{tag}'
    try:
        release = json.loads(shared.github_metadata(endpoint))
    except (OSError, subprocess.SubprocessError, ValueError):
        # Preserve GitHub's public digest fallback when the unauthenticated API
        # is rate limited. Download size is then recorded after hash validation.
        class Assets(HTMLParser):
            def __init__(self):
                super().__init__()
                self.checksums = []
                self.link = False

            def handle_starttag(self, tag_name, attrs):
                attrs = dict(attrs)
                if tag_name == 'a' and attrs.get('href') == f'/{repository}/releases/download/{tag}/{filename}':
                    self.link = True
                if tag_name == 'clipboard-copy' and attrs.get('aria-label') == 'Copy to clipboard digest for '+filename:
                    self.checksums.append(attrs.get('value', ''))

        page = shared.github_metadata(f'https://github.com/{repository}/releases/expanded_assets/{tag}')
        assets = Assets()
        assets.feed(page.decode('utf-8'))
        if not assets.link or len(assets.checksums) != 1 or not re.fullmatch('sha256:[0-9a-f]{64}', assets.checksums[0]):
            raise ValueError('Runtime release has no verified digest: '+name)
        checksum = assets.checksums[0][7:]
        return dict(name=name, file=name+'-'+checksum+'.tar.gz', asset=filename,
                    sha256=checksum, bytes=None,
                    url=f'https://github.com/{repository}/releases/download/{tag}/{filename}')
    if release.get('draft') or release.get('tag_name') != tag:
        raise ValueError('Invalid runtime release')
    asset = next((a for a in release.get('assets', []) if a.get('name') == filename), None)
    if (not asset or asset.get('state') != 'uploaded'
            or not isinstance(asset.get('digest'), str)
            or not re.fullmatch('sha256:[0-9a-f]{64}', asset['digest'])
            or type(asset.get('size')) is not int or asset['size'] <= 0
            or type(asset.get('id')) is not int or asset['id'] <= 0):
        raise ValueError('Runtime release has no verified asset: '+name)
    checksum = asset['digest'][7:]
    return dict(name=name, file=name+'-'+checksum+'.tar.gz', asset=filename,
                sha256=checksum, bytes=asset['size'],
                url=f'https://github.com/{repository}/releases/download/{tag}/{filename}')


def host_path(path):
    """Map guest files to Android backing files (including per-instance HOME)."""
    path = Path(path).resolve()
    files = Path(os.environ['BIONICX_FILES']).resolve()
    if path.is_relative_to('/arlinux-shared'):
        return files/'arlinux-shared'/path.relative_to('/arlinux-shared')
    if path.is_relative_to(files):
        return path
    return Path(os.environ['BIONICX_ROOTFS'])/str(path).lstrip('/')


def guest_path(path):
    files = Path(os.environ['BIONICX_FILES']).resolve()
    path = Path(path)
    if path.is_relative_to(files/'arlinux-shared'):
        return Path('/arlinux-shared')/path.relative_to(files/'arlinux-shared')
    root = Path(os.environ['BIONICX_ROOTFS'])
    if path.is_relative_to(root):
        return Path('/')/path.relative_to(root)
    return path


def environment(root, prefix):
    # These are guest adapter settings, never a command for executing native Wine.
    env = desktop_bus.environment(os.environ)
    env.update(ARLINUX_WINDOWS_RUNTIME=root.name, ARLINUX_WINDOWS_PREFIX=str(prefix),
               WINEPREFIX=str(prefix/'pfx'))
    return env


def request(root, prefix, executable, arguments=(), overrides='', stdio=False, diagnostics=None, timeout=300, task=True):
    exe = str(executable)
    if exe.startswith('/'):
        exe = str(host_path(exe))
    argv = [str(host_path(value)) if str(value).startswith('/') and
        not re.fullmatch('/[A-Za-z][A-Za-z0-9:_-]*', str(value)) else str(value)
        for value in arguments]
    payload = dict(runtime=root.name, prefix=str(host_path(prefix/'pfx')), exe=exe,
                   args=argv, dllOverrides=overrides, stdio=stdio, task=task)
    if diagnostics:
        payload['environment'] = diagnostics
    return _host_request(payload, timeout, stdio)


def _host_request(payload, timeout=10, stdio=False):
    files = Path(os.environ['BIONICX_FILES']).resolve()
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    try:
        client.connect('\0arlinux-windows-' + str(files.stat().st_uid))
        client.sendall(json.dumps(payload).encode()+b'\n')
        line = bytearray()
        while len(line) <= 32768:
            byte = client.recv(1)
            if not byte:
                raise RuntimeError('Android Windows host disconnected')
            if byte == b'\n':
                break
            line.extend(byte)
        else:
            raise RuntimeError('Windows host response too large')
        response = json.loads(line)
        if 'error' in response:
            raise RuntimeError(response['error'])
        if stdio:
            client.settimeout(None)
            return response, client
        client.close()
        return response
    except BaseException:
        client.close()
        raise


def initialize(root, prefix, env=None):
    # The native host invokes the Wine-owned initializer before this command.
    run(root, prefix, 'cmd.exe', ['/c', 'exit', '0'], env=env, check=True)


def cancel_job(job):
    """Stop one recorded launcher, not its shared wineserver or detached children."""
    if not isinstance(job, str) or not re.fullmatch(r'[0-9a-f-]{36}', job):
        raise ValueError('Invalid Windows job identity')
    result = _host_request({'operation': 'cancel-job', 'job': job})
    if result.get('ok') is not True or result.get('scope') != 'launcher':
        raise RuntimeError('Windows host did not acknowledge launcher cancellation')


class WindowsWaitTimeout(TimeoutError):
    """Waiting expired; ownership is explicit and the task has not been cancelled."""
    def __init__(self, response):
        self.job = response['job']
        self.log = guest_path(response['log'])
        super().__init__(f'Windows wait timed out; job {self.job} may still be running; log: {self.log}')

    def cancel_launcher(self):
        cancel_job(self.job)


def run(root, prefix, executable, arguments=(), env=None, check=False, capture=False, timeout=600, task=False):
    """Wait for a command; interactive tasks also retain their child processes."""
    env = env or {}
    diagnostics = {name: env[name] for name in ('WINEDEBUG', 'ARLINUX_EXCEPTION_LOG', 'ARLINUX_VULKAN_CAPTURE',
        'SteamAppId', 'SteamGameId', 'SteamOverlayGameId',
        'WEBVIEW2_BROWSER_EXECUTABLE_FOLDER', 'WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS') if name in env}
    response = request(root, prefix, executable, arguments, env.get('WINEDLLOVERRIDES', ''),
                       diagnostics=diagnostics, task=task)
    deadline = time.monotonic()+timeout if timeout is not None else None
    exit_path = guest_path(response['exit'])
    while not exit_path.is_file():
        if deadline is not None and time.monotonic() >= deadline:
            raise WindowsWaitTimeout(response)
        time.sleep(.2)
    output = guest_path(response['log']).read_text(errors='replace')
    code = int(exit_path.read_text())
    if not capture and output:
        print(output, end='', flush=True)
    result = subprocess.CompletedProcess([str(executable), *arguments], code, output, '')
    if check:
        result.check_returncode()
    return result


def dos_path(path):
    """Translate a guest filename using the installed prefix's real Wine drives.

    Accepts a not-yet-created Save As filename; its parent must already exist.
    Shell dialogs may not traverse Android's protected Z: ancestors even when
    direct file I/O succeeds. Never guess a drive letter from the Unix path.
    """
    path = Path(path).expanduser().resolve()
    if not path.parent.is_dir():
        raise FileNotFoundError('Windows destination directory does not exist: '+str(path.parent))
    result = run(runtime(), instance_prefix(), 'winepath.exe',
                 ['-w', str(host_path(path))], check=True, capture=True, timeout=15)
    translated = result.stdout.strip()
    if not re.fullmatch(r'[A-Za-z]:\\[^\r\n]*', translated):
        raise RuntimeError('Wine did not return a single DOS filename')
    return translated


def launch(executable, arguments=(), prefix=None, dll_overrides=''):
    executable = executable.resolve(strict=True)
    root = ensure_runtime()
    if prefix is not None and prefix.resolve() != instance_prefix().resolve():
        raise ValueError('Bionic Wine uses one instance prefix')
    prefix = instance_prefix()
    env = environment(root, prefix)
    if dll_overrides:
        env['WINEDLLOVERRIDES'] = dll_overrides
    import windows_a11y
    initialize(root, prefix, env)
    import windows_webview
    env = windows_webview.prepare(root, prefix, env)
    windows_a11y.ensure(root, prefix, executable.stem, env)
    if executable.suffix.lower() == '.lnk':
        return run(root, prefix, 'start.exe', [str(executable), *arguments], env=env, timeout=None, task=True).returncode
    return run(root, prefix, executable, arguments, env=env, timeout=None, task=True).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, help='Wine data directory (contains pfx)')
    parser.add_argument('--dll-overrides', default='', help='DLL overrides for this application process')
    parser.add_argument('executable', nargs='?', type=Path)
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.executable:
        parser.error('Provide a Windows executable')
    return launch(args.executable, args.arguments, args.prefix, args.dll_overrides)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, RuntimeError, tarfile.TarError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'arlinux-windows: {error}')
