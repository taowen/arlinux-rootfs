"""Prepare graphics libraries for Valve's official FEX compatibility tool.

Steam owns compatibility selection and game startup. This module supplies
a private graphics provider; it never launches games or modifies Valve files.
"""
import fcntl
import filecmp
import gzip
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def remove_old_tool(steam):
    """Remove only our former tool and its selections, while Steam is stopped."""
    pidfile = Path.home()/'.steam/steam.pid'
    if pidfile.is_file():
        try:
            pid = int(pidfile.read_text().strip())
            if pid <= 0:
                raise ValueError('Invalid Steam PID')
            os.kill(pid, 0)
            return
        except ProcessLookupError:
            pass
        except (ValueError, PermissionError):
            raise RuntimeError('Cannot safely determine whether Steam is running')
    path = steam/'config/config.vdf'
    from steam import keyvalues
    data = keyvalues(path.read_text()) if path.exists() else {}
    node = data
    for name in ('InstallConfigStore', 'Software', 'Valve', 'Steam', 'CompatToolMapping'):
        node = node.get(name, {})
    removed = [key for key, value in node.items()
               if isinstance(value, dict) and value.get('name') == 'arlinux-fex']
    if removed:
        for key in removed:
            del node[key]
        def dump(block, indent=0):
            lines = []
            for key, value in block.items():
                prefix = '\t'*indent + json.dumps(key, ensure_ascii=False)
                if isinstance(value, dict):
                    lines += [prefix, '\t'*indent+'{', dump(value, indent+1), '\t'*indent+'}']
                else:
                    lines.append(prefix+'\t'+json.dumps(value, ensure_ascii=False))
            return '\n'.join(lines)
        backup = path.with_suffix('.before-official-fex')
        if not backup.exists():
            shutil.copy2(path, backup)
        temporary = path.with_suffix('.arlinux-tmp')
        temporary.write_text(dump(data)+'\n')
        temporary.replace(path)
    tool = steam/'compatibilitytools.d/arlinux-fex'
    # Preserve unexpected/user-added content instead of recursively deleting it.
    script = tool/'run'
    if script.is_file() and '/usr/lib/arlinux/steam_fex.py' in script.read_text():
        for name in ('run', 'toolmanifest.vdf', 'compatibilitytool.vdf'):
            (tool/name).unlink(missing_ok=True)
        if not any(tool.iterdir()):
            tool.rmdir()


def configure(steam):
    remove_old_tool(steam)
    os.environ.setdefault('PRESSURE_VESSEL_BWRAP', '/usr/local/bin/bwrap')
    fex, root = prepare(steam)
    os.environ.setdefault('STEAM_COMPAT_GRAPHICS_PROVIDER', str(root/'graphics_provider.json'))
    os.environ.setdefault('STEAM_COMPAT_FEX_CONFIG',
                          'TSOEnabled:1,Multiblock:1,ThunksDB_GL:1,ThunksDB_Vulkan:1')
    os.environ.setdefault('FEX_ROOTFS', str(root))
    libs = root/'usr/lib/x86_64-linux-gnu'
    os.environ.setdefault('FEX_ENV', 'LD_LIBRARY_PATH='
                          + os.environ.get('LD_LIBRARY_PATH', '') + ':'
                          + str(libs) + ':' + str(libs/'pulseaudio'))
    # Export auxiliary dlopen resources as well as the ELF dependencies.
    # Resolve the app-owned root alias once, before entering Valve's path view.
    files = os.environ.get('BIONICX_FILES')
    if files:
        native = (Path(files)/'rootfs').resolve(strict=True)
        hybris = native/'usr/lib/hybris'
        directories = [native/'usr/lib/mesa', hybris,
                       *map(Path, ('/system', '/system_ext', '/product', '/vendor', '/odm', '/apex'))]
        mounts = os.environ.get('STEAM_COMPAT_MOUNTS', '').split(':')
        mounts += [str(p) for p in directories if p.is_dir()]
        os.environ['STEAM_COMPAT_MOUNTS'] = ':'.join(dict.fromkeys(p for p in mounts if p))
        if hybris.is_dir():
            os.environ.setdefault('HYBRIS_LINKER_DIR', str(hybris/'libhybris/linker'))


class IncompleteRuntime(RuntimeError):
    pass


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
    raise IncompleteRuntime(f'Install {name} through Steam first: steam://install/{appid}')


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
        raise IncompleteRuntime('Steam Linux Runtime 3.0 (sniper) is incomplete; verify its files in Steam')
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
            (stage/'complete').touch()
            stage.rename(root)
        # pressure-vessel rejects provider links outside the provider root.
        # Copy Valve's thunks into our cache, refreshing them after FEX updates.
        # Neither the FEX depot nor the Steam runtime depot is modified.
        for name, thunk in [('libGL.so.1', 'libGL-guest.so'),
                            ('libvulkan.so.1', 'libvulkan-guest.so')]:
            source = fex/'usr/share/fex-emu/GuestThunks'/thunk
            target = root/'usr/lib/x86_64-linux-gnu'/name
            if target.is_symlink() or not target.is_file() or not filecmp.cmp(source, target, shallow=False):
                temporary = target.with_suffix(target.suffix + '.tmp')
                shutil.copy2(source, temporary)
                temporary.replace(target)
        provider = {'graphics_provider_v0': {
            'root': './', 'locales': False, 'va_api': False, 'vdpau': False,
            'architectures': {'x86_64-linux-gnu': {
                'fallback_library_paths': ['/usr/lib/x86_64-linux-gnu'],
                'gconv': '/usr/lib/x86_64-linux-gnu/gconv'}}}}
        descriptor = root/'graphics_provider.json'
        content = json.dumps(provider)+'\n'
        if not descriptor.exists() or descriptor.read_text() != content:
            temporary = descriptor.with_suffix('.tmp')
            temporary.write_text(content)
            temporary.replace(descriptor)
    return fex, root
