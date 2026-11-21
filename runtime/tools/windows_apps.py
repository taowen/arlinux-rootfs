"""On-demand Windows apps sharing the instance Bionic Wine prefix.

Downloads come from the vendors, never from a copied development prefix.
The common WebView2 sandbox is disabled in the shared instance Wine prefix:
Wine lacks the required Windows sandbox interception thunks. Android app
isolation is not equivalent to browser sandboxing. First installation requires
explicit consent. These applications are experimental on ARLinux.
"""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

import windows
import windows_webview
import shared


MANIFEST = Path('/usr/lib/arlinux/guest/windows-app-downloads.tsv')
LICENSES = {
    'paintnet': 'https://www.getpaint.net/license.html',
    'powerbi': 'https://powerbi.microsoft.com/desktop-eula/',
    'fontforge': 'https://fontforge.org/en-US/license/',
}


def download(name):
    row = next(line.split('\t') for line in MANIFEST.read_text().splitlines()
               if line.startswith(name + '\t'))
    _, expected, url = row
    if (not re.fullmatch('[0-9a-f]{64}', expected) or not url.startswith('https://')):
        raise ValueError('Invalid Windows application download manifest')
    return shared.download(url, expected, '.zip' if name == 'paintnet' else '.exe')


def unpack_zip(archive, destination):
    """Normalize vendor Windows ZIP paths without allowing traversal or links."""
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            name = entry.filename.replace('\\', '/')
            target = (destination / name).resolve()
            if (not target.is_relative_to(destination.resolve()) or ':' in name
                    or (entry.external_attr >> 16) & 0o170000 == 0o120000):
                raise ValueError(f'Unsafe ZIP entry: {name}')
            if entry.is_dir() or name.endswith('/'):
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(entry) as data, target.open('wb') as output:
                    shutil.copyfileobj(data, output)


def consent(app, accepted):
    print(f'First installation needs internet access and several GB of storage.\n'
          f'License: {LICENSES[app]}', flush=True)
    if app == 'powerbi':
        print('Also installs Microsoft .NET and WebView2 under their licenses.\n'
              'WebView2 browser sandboxing is disabled inside this Wine prefix.\n'
              'Use trusted reports and data sources only.', flush=True)
    else:
        print('The vendor Wine build is experimental and expires after 12 weeks.', flush=True)
    if not accepted and (not sys.stdin.isatty() or
                         input('Accept the licenses and continue? [y/N] ').strip().lower() != 'y'):
        raise RuntimeError('Installation cancelled; no application license was accepted')


def restore_powerbi_accessibility(root, prefix):
    """Restore the hybrid Wine UIA DLL overwritten by the x64 vendor setup.

    ARM64 WebView2 cannot load the vendor's x64-only system DLL. Wine's
    ARM64X builtin also serves Power BI's emulated x64 clients.
    """
    source = root / 'files/lib/wine/aarch64-windows/uiautomationcore.dll'
    data = source.read_bytes()
    if b'Wine builtin DLL' not in data[:256]:
        raise RuntimeError('Matching Wine UIAutomationCore builtin is missing')
    destination = prefix / 'pfx/drive_c/windows/system32/uiautomationcore.dll'
    temporary = destination.with_suffix('.arlinux-tmp')
    temporary.write_bytes(data)
    temporary.replace(destination)


@contextlib.contextmanager
def installer_windows_version(root, prefix, names, version):
    """Temporarily select installer compatibility without changing the desktop OS."""
    reg = prefix / 'pfx/drive_c/windows/system32/reg.exe'
    def registry(*args, check=True):
        return windows.run(root, prefix, reg, args, capture=True, check=check, timeout=60)

    def restore(key, value):
        if value is None:
            result = registry('delete', key, '/v', 'Version', '/f', check=False)
            if result.returncode not in (0, 1):
                result.check_returncode()
        else:
            registry('add', key, '/v', 'Version', '/t', 'REG_SZ', '/d', value, '/f')

    with contextlib.ExitStack() as cleanup:
        for name in dict.fromkeys(names):
            key = r'HKCU\Software\Wine\AppDefaults' + '\\' + name
            result = registry('query', key, '/v', 'Version', check=False)
            if result.returncode not in (0, 1):
                result.check_returncode()
            match = re.search(r'Version\s+REG_SZ\s+(\S+)', result.stdout)
            if result.returncode == 0 and not match:
                raise RuntimeError('Cannot preserve installer Windows version: ' + name)
            cleanup.callback(restore, key, match[1] if match else None)
            registry('add', key, '/v', 'Version', '/t', 'REG_SZ', '/d', version, '/f')
        yield


def install_powerbi(root, prefix):
    env = windows.environment(root, prefix)
    system = prefix / 'pfx/drive_c/windows/system32'

    def run(exe, *args, **kwargs):
        result = windows.run(root, prefix, exe, args,
                             env={**env, **kwargs.get('environment', {})},
                             capture=kwargs.get('capture', False), timeout=3600)
        # Windows installers return 3010 for successful installation requiring
        # reboot; Wine's Unix exit status carries its low byte.
        if result.returncode not in (0, 3010 & 255):
            result.check_returncode()
        if result.returncode == (3010 & 255):
            # Complete delayed CLR replacements before the next installer loads
            # them. Kill this isolated prefix's processes first: wineboot would
            # otherwise discard pending renames for DLLs still held open.
            windows.run(root, prefix, system / 'wineboot.exe', ['-k', '-r'],
                        env={**env, 'WINEDLLOVERRIDES': 'mscoree=b'},
                        timeout=3600, check=True)
        return result

    reg = system / 'reg.exe'
    # Remove Wine Mono through its registered uninstaller, not fabricated keys.
    listing = run(system / 'uninstaller.exe', '--list', capture=True)
    for line in listing.stdout.splitlines():
        match = re.fullmatch(r'([^|]+)\|\|\|Wine Mono(?: .*)?', line.strip())
        if match:
            run(system / 'uninstaller.exe', '--remove', match[1])
    # A missing key is expected before installation or after an interrupted
    # install. Probe it without treating reg.exe's not-found status as fatal.
    release = windows.run(root, prefix, reg,
                          ['query', r'HKLM\Software\Microsoft\NET Framework Setup\NDP\v4\Full',
                           '/v', 'Release', '/reg:64'], env=env, capture=True, timeout=3600)
    if release.returncode not in (0, 1):
        release.check_returncode()
    match = re.search(r'Release\s+REG_DWORD\s+(0x[0-9a-fA-F]+)', release.stdout)
    # Wine's default registry advertises .NET 4.8 even without Microsoft's CLR.
    # A version key alone is therefore not an installed dependency check.
    native_clr = prefix / 'pfx/drive_c/windows/Microsoft.NET/Framework64/v4.0.30319/clr.dll'
    native_shims = [prefix / 'pfx/drive_c/windows' / directory / 'mscoree.dll'
                    for directory in ('system32', 'syswow64')]
    # .NET 4.8 is an upgrade: its success does not install the CLR activation
    # shim when 4.0 was skipped because Wine advertised a default NDP version.
    def microsoft_shims():
        return all(path.is_file() and b'Wine builtin DLL' not in path.read_bytes()[:256]
                   for path in native_shims)

    if not native_clr.is_file() or not microsoft_shims() or not match or int(match[1], 16) < 528040:
        if not native_clr.is_file() or not microsoft_shims():
            print('Installing Microsoft .NET 4.0…', flush=True)
            # Remove Wine's placeholder registration only when the real CLR
            # is absent. Do not claim an installed Microsoft version.
            if not native_clr.is_file():
                for view in ('/reg:32', '/reg:64'):
                    key = r'HKLM\Software\Microsoft\NET Framework Setup\NDP\v4'
                    existing = windows.run(root, prefix, reg, ['query', key, view],
                                           env=env, capture=True)
                    if existing.returncode == 0:
                        run(reg, 'delete', key, '/f', view)
            installer = download('dotnet40')
            # As in Winetricks' dotnet40 recipe, XP compatibility includes the
            # native CLR activation shims that modern Windows already supplies.
            # Scope it to installer processes and restore before .NET 4.8;
            # the shared desktop and unrelated applications retain their OS.
            with installer_windows_version(root, prefix,
                    (installer.name, 'Setup.exe', 'msiexec.exe'), 'winxp'):
                run(installer, '/q', '/norestart', '/log', r'C:\dotnet40.log',
                    environment={'WINEDLLOVERRIDES': 'fusion=b'})
            if not microsoft_shims():
                raise RuntimeError('Microsoft .NET 4.0 did not install both CLR activation shims')
            run(reg, 'add', r'HKCU\Software\Wine\DllOverrides', '/v', 'mscoree',
                '/t', 'REG_SZ', '/d', 'native,builtin', '/f')
        print('Installing Microsoft .NET 4.8…', flush=True)
        installer = download('dotnet48')
        # Win10 installers assume the OS already includes .NET 4.8. Use the
        # Winetricks Win7 installation path without changing the desktop OS.
        with installer_windows_version(root, prefix,
                (installer.name, 'Setup.exe', 'msiexec.exe'), 'win7'):
            run(installer, '/q', '/norestart', '/log', r'C:\dotnet48.log',
                environment={'WINEDLLOVERRIDES': 'fusion=b'})
        release = windows.run(root, prefix, reg,
                              ['query', r'HKLM\Software\Microsoft\NET Framework Setup\NDP\v4\Full',
                               '/v', 'Release', '/reg:64'], env=env, capture=True, timeout=3600)
        if release.returncode not in (0, 1):
            release.check_returncode()
        match = re.search(r'Release\s+REG_DWORD\s+(0x[0-9a-fA-F]+)', release.stdout)
        if not native_clr.is_file() or not microsoft_shims() or not match or int(match[1], 16) < 528040:
            raise RuntimeError('Microsoft .NET 4.8 installation did not complete')
    print('Installing Power BI Desktop…', flush=True)
    run(download('powerbi'), '-quiet', '-norestart', 'ACCEPT_EULA=1', 'ENABLECXP=0')
    executable = prefix / 'pfx/drive_c/Program Files/Microsoft Power BI Desktop/bin/PBIDesktop.exe'
    if not executable.is_file():
        raise RuntimeError('Power BI installer did not create PBIDesktop.exe; see installer logs in the prefix')
    restore_powerbi_accessibility(root, prefix)
    # Re-register oleacc after installers so IAccessible marshaling uses the
    # Accessibility typelib instead of an imported UIAutomation mapping.
    run(reg.parent / 'regsvr32.exe', '/s', 'oleacc.dll')
    return executable


def ensure_powerbi_authentication():
    # Offline reports also authenticate against the local Analysis Services.
    # Android's Bionic Wine host invokes this helper through the guest launcher.
    if shutil.which('ntlm_auth'):
        return
    print('Installing local authentication support…', flush=True)
    subprocess.run(['sudo', 'apt-get', 'update'], check=True)
    subprocess.run(['sudo', 'apt-get', 'install', '-y', '--no-install-recommends',
                    'winbind'], check=True)


def fontforge_executable(prefix):
    # The official x64 installer uses Program Files (x86). Accept a manually
    # installed copy too, without requiring our installed.json marker.
    for directory in ('Program Files (x86)', 'Program Files'):
        application = prefix / 'pfx/drive_c' / directory / 'FontForgeBuilds'
        executable = application / 'run_fontforge.exe'
        if executable.is_file() and (application / 'bin/fontforge.exe').is_file():
            return executable
    return None


def install_fontforge(root, prefix):
    print('Installing FontForge…', flush=True)
    windows.run(root, prefix, download('fontforge'),
                ['/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-'],
                env=windows.environment(root, prefix), timeout=3600, check=True)
    executable = fontforge_executable(prefix)
    if executable is None:
        raise RuntimeError('FontForge installer did not create the application; click again to retry')
    return executable


def battle_net_launcher(root, prefix):
    default = prefix / 'pfx/drive_c/Program Files (x86)/Battle.net/Battle.net Launcher.exe'
    if default.is_file():
        return default
    # Battle.net registers this handler even when installed outside Program Files.
    result = windows.run(root, prefix, 'reg.exe',
                         ['query', r'HKCR\battlenet\shell\open\command', '/ve'],
                         capture=True, timeout=15)
    match = re.search(r'REG_SZ\s+"([^"\r\n]+\\Battle\.net\.exe)"',
                      result.stdout, re.IGNORECASE)
    if result.returncode or not match:
        return None
    path = windows.run(root, prefix, 'winepath.exe', ['-u', match[1]],
                       capture=True, timeout=15)
    location = path.stdout.strip()
    if path.returncode or not location.startswith('/') or '\n' in location:
        return None
    executable = windows.guest_path(location)
    launcher = executable.with_name('Battle.net Launcher.exe')
    if launcher.is_file():
        return launcher
    return executable if executable.is_file() else None


def launch_world_of_warcraft():
    prefix = windows.instance_prefix()
    root = windows.ensure_runtime()
    windows.initialize(root, prefix, windows.environment(root, prefix))
    with shared.locked(prefix / 'battle-net-install.lock'):
        launcher = battle_net_launcher(root, prefix)
        if launcher is None:
            print('Downloading the official Battle.net installer…', flush=True)
            # Wait for Setup itself. Its spawned client can stay open after installation.
            windows.run(root, prefix, download('battlenet'), timeout=3600, check=True)
            launcher = battle_net_launcher(root, prefix)
            if launcher is None:
                raise RuntimeError('Battle.net setup did not finish')
    # Battle.net chooses whether the installed game can start or needs setup.
    return windows.run(root, prefix, launcher, ['battlenet://WoW', '--exec=launch WoW'],
                       timeout=None, task=True).returncode


def prepare(app, accepted=False):
    home = windows.storage() / 'apps' / app
    home.mkdir(parents=True, exist_ok=True)
    cache = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'arlinux/windows-apps' / app
    cache.mkdir(parents=True, exist_ok=True)
    prefix = windows.instance_prefix() if hasattr(windows, 'instance_prefix') else home / 'wine'
    pointer = home / 'installed.json'
    pointer_root = windows.storage() if hasattr(windows, 'instance_prefix') else home
    with (home / 'install.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if app == 'fontforge':
            executable = fontforge_executable(prefix)
            if executable is not None:
                return executable, prefix, cache
        if app != 'fontforge' and pointer.is_file():
            entry = json.loads(pointer.read_text())
            executable = (pointer_root / entry['executable']).resolve(strict=True)
            if not executable.is_relative_to(pointer_root.resolve()):
                raise ValueError('Invalid installed app path')
            if app == 'powerbi':
                ensure_powerbi_authentication()
            return executable, prefix, cache
        # FontForge is freely licensed; opening its icon starts installation.
        if app != 'fontforge':
            consent(app, accepted)
        if app == 'powerbi':
            ensure_powerbi_authentication()
        root = windows.ensure_runtime()
        windows.initialize(root, prefix, windows.environment(root, prefix))
        if app == 'paintnet':
            # Atomic extraction leaves retryable downloads, not half-ready apps.
            application = prefix / 'pfx/drive_c/Program Files/Paint.NET'
            if not (application / 'paintdotnet.exe').is_file():
                with tempfile.TemporaryDirectory(prefix='extract-', dir=home) as temporary:
                    stage = Path(temporary) / 'application'
                    stage.mkdir()
                    unpack_zip(download(app), stage)
                    if not (stage / 'paintdotnet.exe').is_file():
                        raise RuntimeError('Paint.NET executable missing from official archive')
                    application.parent.mkdir(parents=True, exist_ok=True)
                    stage.rename(application)
            executable = application / 'paintdotnet.exe'
        elif app == 'fontforge':
            executable = install_fontforge(root, prefix)
        else:
            executable = install_powerbi(root, prefix)
        temporary = pointer.with_suffix('.tmp')
        temporary.write_text(json.dumps({'executable': str(executable.relative_to(pointer_root))}) + '\n')
        temporary.replace(pointer)
        return executable, prefix, cache


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', choices=(*LICENSES, 'wow'))
    parser.add_argument('--desktop', action='store_true')
    parser.add_argument('--accept-license', action='store_true', help='Explicit consent for unattended installation')
    args = parser.parse_args()
    if args.app == 'wow':
        return launch_world_of_warcraft()
    executable, prefix, cache = prepare(args.app, args.accept_license)
    root = windows.ensure_runtime()
    env = windows.environment(root, prefix)
    windows.initialize(root, prefix, env)
    env = windows_webview.prepare(root, prefix, env)
    import windows_a11y
    windows_a11y.ensure(root, prefix, {'paintnet': 'Paint.NET', 'powerbi': 'Power BI Desktop', 'fontforge': 'FontForge'}[args.app], env)
    if args.app == 'paintnet':
        # These are the vendor paintdotnet.sh overrides, not modified binaries.
        env['WINEDLLOVERRIDES'] = 'mshtml=;d3dcompiler_47=n'
    response = windows.request(root, prefix, executable,
                               overrides=env.get('WINEDLLOVERRIDES', ''),
                               diagnostics={key: env[key] for key in (
                                   'WEBVIEW2_BROWSER_EXECUTABLE_FOLDER',
                                   'WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS') if key in env})
    (cache / 'desktop.log').write_text('Native log: '+response['log']+'\n')
    exit_file = windows.guest_path(response['exit'])
    import time
    time.sleep(2)
    if exit_file.is_file() and int(exit_file.read_text()) != 0:
        raise RuntimeError('Application exited; see '+str(windows.guest_path(response['log'])))
    return 0


if __name__ == '__main__':
    try:
        status = main()
    except (OSError, ValueError, RuntimeError, KeyError, StopIteration,
            zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f'Windows app: {error}\nClick the app again to retry.', file=sys.stderr)
        status = 1
    if status and '--desktop' in sys.argv and sys.stdin.isatty():
        try:
            input('\nPress Enter to close this window. ')
        except (EOFError, KeyboardInterrupt):
            pass
    sys.exit(status)
