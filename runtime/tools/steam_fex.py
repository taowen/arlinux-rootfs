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
import shared
import hashlib


def configure(steam):
    os.environ.setdefault('PRESSURE_VESSEL_BWRAP', '/usr/local/bin/bwrap')
    configure_graphics()
    fex, root = prepare(steam)
    # We launch Valve's ARM64 client directly. Its normal shell launcher adds
    # these native runtime helpers to PATH; the client starts the launcher
    # service by name, so leaving them out disables it even with sniper installed.
    runtime_bin = graphics_runtime(steam)[0]/'pressure-vessel-arm64/bin'
    paths = os.environ.get('PATH', os.defpath).split(os.pathsep)
    os.environ['PATH'] = os.pathsep.join(dict.fromkeys([str(runtime_bin), *paths]))
    os.environ.setdefault('STEAM_COMPAT_GRAPHICS_PROVIDER', str(root/'graphics_provider.json'))
    os.environ.setdefault('STEAM_COMPAT_FEX_CONFIG',
                          'TSOEnabled:1,Multiblock:1,ThunksDB_GL:1,ThunksDB_Vulkan:1')
    os.environ.setdefault('FEX_ROOTFS', str(root))
    libs = root/'usr/lib/x86_64-linux-gnu'
    os.environ.setdefault('FEX_ENV', 'LD_LIBRARY_PATH='
                          + os.environ.get('LD_LIBRARY_PATH', '') + ':'
                          + str(libs) + ':' + str(libs/'pulseaudio'))


def configure_graphics():
    """Keep the native graphics runtime addressable in pressure-vessel's view.

    The host's rootfs alias is outside the container's root. Resolve managed
    graphics paths before pressure-vessel inspects them, and export that same
    image as a single mount: ICD/layer manifests have dependencies beyond Mesa.
    Explicit graphics paths outside the managed image remain unchanged.
    """
    files = os.environ.get('BIONICX_FILES')
    if not files:
        return
    alias = Path(files)/'rootfs'
    native = alias.resolve(strict=True)
    prefix = str(alias) + '/'
    for key in ('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_LAYER_PATH',
                'LIBGL_DRIVERS_PATH', 'LD_LIBRARY_PATH'):
        if key in os.environ:
            os.environ[key] = ':'.join(str(native/path[len(prefix):])
                                      if path.startswith(prefix) else path
                                      for path in os.environ[key].split(':'))
    directories = [native, *map(Path, ('/arlinux-shared', '/system', '/system_ext',
                                     '/product', '/vendor', '/odm', '/apex'))]
    mounts = os.environ.get('STEAM_COMPAT_MOUNTS', '').split(':')
    mounts += [str(p) for p in directories if p.is_dir()]
    os.environ['STEAM_COMPAT_MOUNTS'] = ':'.join(dict.fromkeys(p for p in mounts if p))
    hybris = native/'usr/lib/hybris'
    if hybris.is_dir():
        os.environ.setdefault('HYBRIS_LINKER_DIR', str(hybris/'libhybris/linker'))


class IncompleteRuntime(RuntimeError):
    pass


def graphics_runtime(steam):
    """Use a complete Valve runtime, including Scout's Soldier container."""
    for appid, name in (('1628350', 'sniper'), ('1391110', 'soldier')):
        try:
            runtime = app_directory(steam, appid)
        except IncompleteRuntime:
            continue
        platforms = [p for p in runtime.glob(name+'_platform_*')
                     if (p/'usr-mtree.txt.gz').is_file()]
        if platforms:
            return runtime, max(platforms, key=lambda p: tuple(map(int, re.findall(r'\d+', p.name))))
    raise IncompleteRuntime('Install a complete Steam Linux Runtime (Sniper or Soldier) through Steam')


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
        chinese = shared.acceleration_enabled()
        mirror = 'https://mirrors.tuna.tsinghua.edu.cn/debian' if chinese else 'https://deb.debian.org/debian'
        source.write_text('deb [arch=amd64 signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] '
                          + mirror + ' bookworm main\n')
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
    _, platform = graphics_runtime(steam)
    cache = shared.directory('steam-fex')
    # Never overwrite thunks in a provider used by another instance's process.
    identity = hashlib.sha256()
    identity.update((platform/'usr-mtree.txt.gz').read_bytes())
    for thunk in ('libGL-guest.so', 'libvulkan-guest.so'):
        identity.update((fex/'usr/share/fex-emu/GuestThunks'/thunk).read_bytes())
    root = cache/(platform.name+'-v2-'+identity.hexdigest()[:24])
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
