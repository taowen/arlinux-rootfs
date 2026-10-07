"""Steam compatibility tool for native x86-64 Linux games on ARM64.

Uses Valve's FEX and sniper depots, without pressure-vessel's unavailable
user namespaces. Guest libraries remain in a private cache, never /usr/lib.
"""
import fcntl
import gzip
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def register(steam):
    tool = steam/'compatibilitytools.d/arlinux-fex'
    tool.mkdir(parents=True, exist_ok=True)
    files = {
        'compatibilitytool.vdf': '''"compatibilitytools" { "compat_tools" {
 "arlinux-fex" { "install_path" "." "display_name" "ARLinux Linux x86-64 (FEX)"
 "from_oslist" "linux" "to_oslist" "linux" }
} }
''',
        'toolmanifest.vdf': '''"manifest" { "version" "2"
 "commandline" "/run %verb%" "use_tool_subprocess_reaper" "1" }
''',
        'run': '#!/bin/sh\nexec python3 /usr/lib/arlinux/steam_fex.py "$@"\n',
    }
    for name, content in files.items():
        path = tool/name
        if not path.exists() or path.read_text() != content:
            path.write_text(content)
        path.chmod(0o755 if name == 'run' else 0o644)


def app_directory(steam, appid):
    libraries = [steam]
    listing = steam/'steamapps/libraryfolders.vdf'
    if listing.exists():
        libraries += [Path(p.replace('\\\\', '\\')) for p in
                      re.findall(r'"path"\s+"([^"]+)"', listing.read_text())]
    for library in libraries:
        manifest = library/f'steamapps/appmanifest_{appid}.acf'
        if manifest.exists():
            contents = manifest.read_text()
            state = re.search(r'"StateFlags"\s+"(\d+)"', contents)
            name = re.search(r'"installdir"\s+"([^"]+)"', contents)
            if name and state and int(state[1]) & 4:
                directory = library/'steamapps/common'/name[1]
                if directory.is_dir():
                    return directory
    name = {'3127680': 'FEX-Emu', '1628350': 'Steam Linux Runtime 3.0 (sniper)'}.get(appid, appid)
    raise RuntimeError(f'Install {name} through Steam first: steam://install/{appid}')


def base_packages(cache):
    """APT verifies signed Debian metadata; no host multiarch configuration changes."""
    if not shutil.which('apt-get') or not Path('/usr/share/keyrings/debian-archive-keyring.gpg').is_file():
        raise RuntimeError('Native x86-64 games currently require Debian, Yibu or LXQt')
    # Bookworm's GCC 12 satisfies the GL thunk's GLIBCXX_3.4.29 requirement
    # while staying close to sniper's Debian 11 baseline. Do not mix the
    # desktop's rolling libc with this guest runtime.
    apt = cache/'apt-bookworm'
    apt.mkdir(exist_ok=True)
    packages = apt/'packages'
    packages.mkdir(exist_ok=True)
    marker = packages/'complete'
    if not marker.exists():
        (apt/'lists/partial').mkdir(parents=True, exist_ok=True)
        source = apt/'sources.list'
        source.write_text('deb [arch=amd64 signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] '
                          'https://mirrors.tuna.tsinghua.edu.cn/debian bookworm main\n')
        options = ['-o', 'APT::Architecture=amd64', '-o', 'APT::Architectures=amd64',
                   '-o', f'Dir::Etc::sourcelist={source}', '-o', 'Dir::Etc::sourceparts=-',
                   '-o', f'Dir::State::lists={apt / "lists"}', '-o', 'APT::Sandbox::User=root']
        subprocess.run(['apt-get', *options, 'update'], check=True)
        subprocess.run(['apt-get', *options, 'download', 'libc6:amd64',
                        'libgcc-s1:amd64', 'libstdc++6:amd64'], cwd=packages, check=True)
        marker.touch()
    return sorted(packages.glob('*.deb'))


def materialize(platform, root):
    """Restore Valve's mtree, including content-addressed files and rooted links."""
    usr = root/'usr'
    usr.mkdir(parents=True)
    for name in ('bin', 'sbin', 'lib', 'lib64', 'etc'):
        (root/name).symlink_to('usr/'+name)
    def decode(value):
        return re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), value)
    with gzip.open(platform/'usr-mtree.txt.gz', 'rt') as source:
        for line in source:
            fields = line.split()
            if not fields or fields[0].startswith('#'):
                continue
            relative = Path(decode(fields[0]))
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Unsafe Steam runtime path')
            attrs = dict(p.split('=', 1) for p in fields[1:] if '=' in p)
            path = usr/relative
            kind = attrs.get('type')
            if kind == 'dir':
                path.mkdir(parents=True, exist_ok=True)
            elif kind in ('file', 'link'):
                path.parent.mkdir(parents=True, exist_ok=True)
                if not path.parent.resolve().is_relative_to(root.resolve()):
                    raise ValueError('Steam runtime path escapes its root')
                if kind == 'file':
                    blob = Path(decode(attrs.get('contents', str(relative))))
                    if blob.is_absolute() or '..' in blob.parts:
                        raise ValueError('Unsafe Steam runtime blob')
                    # Copy instead of hardlink: Android may deny linkat, and
                    # package extraction must not overwrite Steam-owned files.
                    if attrs.get('size') == '0':
                        path.touch()
                    else:
                        shutil.copy2(platform/'files'/blob, path)
                    path.chmod(int(attrs.get('mode', '644'), 8))
                else:
                    target = decode(attrs['link'])
                    if target.startswith('/'):
                        target = os.path.relpath(root/target.lstrip('/'), path.parent)
                    if not (path.parent/target).resolve().is_relative_to(root.resolve()):
                        raise ValueError('Steam runtime symlink escapes its root')
                    path.symlink_to(target)


def install_base(packages, root):
    # dpkg-deb may replace /lib symlinks with real directories. Normalize each
    # package into /usr first, retaining a single loader/libc pair in this tree.
    with tempfile.TemporaryDirectory(prefix='base-', dir=root.parent) as directory:
        unpacked = Path(directory)
        for package in packages:
            subprocess.run(['dpkg-deb', '-x', str(package), directory], check=True)
        for directory, dirs, files in os.walk(unpacked):
            for name in files:
                source = Path(directory)/name
                relative = source.relative_to(unpacked)
                if relative.parts[0] in ('lib', 'lib64', 'bin', 'sbin'):
                    relative = Path('usr')/relative
                target = root/relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.unlink(missing_ok=True)
                if source.is_symlink():
                    link = os.readlink(source)
                    if link.startswith('/'):
                        link = os.path.relpath(root/link.lstrip('/'), target.parent)
                    target.symlink_to(link)
                else:
                    shutil.copy2(source, target)
    loader = root/'usr/lib64/ld-linux-x86-64.so.2'
    loader.unlink(missing_ok=True)
    loader.symlink_to('../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2')


def prepare(steam):
    fex = app_directory(steam, '3127680')
    sniper = app_directory(steam, '1628350')
    platforms = [p for p in sniper.glob('sniper_platform_*') if (p/'usr-mtree.txt.gz').is_file()]
    if not platforms:
        raise RuntimeError('Steam Linux Runtime 3.0 (sniper) is incomplete; verify its files in Steam')
    platform = max(platforms, key=lambda p: tuple(map(int, re.findall(r'\d+', p.name))))
    cache = Path(os.environ.get('XDG_CACHE_HOME', Path.home()/'.cache'))/'arlinux/steam-fex'
    cache.mkdir(parents=True, exist_ok=True)
    root = cache/(platform.name+'-v2')
    with (cache/'prepare.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not (root/'complete').exists():
            packages = base_packages(cache)
            stage = cache/(root.name+'.partial')
            if stage.exists():
                # Only our incomplete, named staging directory; no user data.
                shutil.rmtree(stage)
            materialize(platform, stage)
            install_base(packages, stage)
            # FEX's current openat2 path cannot run in an Android app. Expose
            # guest libraries with absolute names in FEX_ENV instead. Thunks
            # need matching paths too; only this private x86 runtime is changed.
            for name, thunk in [('libGL.so.1', 'libGL-guest.so'),
                                ('libvulkan.so.1', 'libvulkan-guest.so')]:
                path = stage/'usr/lib/x86_64-linux-gnu'/name
                path.unlink(missing_ok=True)
                path.symlink_to(fex/'usr/share/fex-emu/GuestThunks'/thunk)
            (stage/'complete').touch()
            stage.rename(root)
        config = root/'app-config.json'
        content = json.dumps({'Config': {'TSOEnabled': '1'},
                              'ThunksDB': {'GL': 1, 'Vulkan': 1}})
        if not config.exists() or config.read_text() != content:
            temporary = config.with_suffix('.tmp')
            temporary.write_text(content)
            temporary.replace(config)
    return fex, root


def launch(verb, command):
    if verb in ('getcompatpath', 'getnativepath'):
        if not command:
            raise ValueError('Missing path')
        print(command[0])
        return 0
    if verb not in ('run', 'waitforexitandrun'):
        return 0
    if command and command[0] == '--':
        command = command[1:]
    if not command:
        raise ValueError('Missing game command')
    steam = (Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'Steam').resolve()
    # Preparation also spawns native APT/dpkg processes, which must not
    # inherit Steam's foreign-architecture overlay.
    os.environ.pop('LD_PRELOAD', None)
    fex, root = prepare(steam)
    env = os.environ.copy()
    # Steam's native ARM64 overlay cannot be preloaded into the x86 process.
    env.pop('LD_PRELOAD', None)
    # Keep the desktop's ARM64 library path for FEX's host-side GL/Vulkan
    # thunks. FEX_ENV overrides it only inside the emulated x86 process.
    if 'ARLINUX_STEAM_HOST_LIBRARY_PATH' in env:
        env['LD_LIBRARY_PATH'] = env.pop('ARLINUX_STEAM_HOST_LIBRARY_PATH')
    env.update(FEX_ROOTFS=str(root), FEX_PORTABLE='1',
               FEX_THUNKHOSTLIBS=str(fex/'usr/lib/aarch64-linux-gnu/fex-emu/HostThunks'),
               FEX_THUNKGUESTLIBS=str(fex/'usr/share/fex-emu/GuestThunks'))
    env['FEX_APP_CONFIG'] = str(root/'app-config.json')
    libs = root/'usr/lib/x86_64-linux-gnu'
    env['FEX_ENV'] = 'LD_LIBRARY_PATH='+str(libs)+':'+str(libs/'pulseaudio')
    os.execve(fex/'usr/bin/FEX', [str(fex/'usr/bin/FEX'), *command], env)


if __name__ == '__main__':
    try:
        sys.exit(launch(sys.argv[1], sys.argv[2:]))
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'ARLinux FEX: {error}', file=sys.stderr)
        sys.exit(1)
