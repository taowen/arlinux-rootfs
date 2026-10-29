#!/usr/bin/env python3
"""Internal hosted-runner adapter; recipes remain behind build.sh."""
from component_recipes import load_plan
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from artifact_download import download
from component_inputs import request_identity, source_inputs


ROOTFS = {'yibu', 'debian', 'arch', 'omarchy', 'plasma', 'lxqt'}
STANDALONE = ROOTFS | {'vulkan-capture', 'guest-tools'}
LINUX_GPU = {'linux-mesa', 'linux-hybris'}

def run(*args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def request():
    data = json.loads(os.environ['ARLINUX_COMPONENT_REQUEST'])
    framework = Path(__file__).resolve().parents[2]
    plan = load_plan(framework/'tools/build')
    stage = plan['stages'].get(data.get('stage'))
    if not stage or os.environ.get('GITHUB_REPOSITORY') != stage['repository']:
        raise ValueError('Component must run in its owning repository')
    for name in ('framework',):
        if not re.fullmatch('[0-9a-f]{40}', data.get(name, '')):
            raise ValueError('Expected a pinned ' + name + ' commit')
    identity = data['identity']
    actual = request_identity(data)
    if identity != actual:
        raise ValueError('Component request identity differs from its inputs')
    if data.get('format') == 2 and data['sourceInputs'] != source_inputs(framework, data['framework'], data['stage'], plan):
        raise ValueError('Pinned checkout differs from the declared component sources')
    expected_arch = 'arm64' if stage['runner'].endswith('-arm') else 'x64'
    if os.environ.get('RUNNER_ARCH', '').lower() != expected_arch:
        raise ValueError('Unexpected hosted runner architecture')
    return data, framework


def prepare(data, framework):
    if os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted':
        raise RuntimeError('Hosted-runner bootstrap cannot run on a workstation')
    root = Path('/opt/arlinux')
    run('sudo', 'mkdir', '-p', root)
    run('sudo', 'chown', '-R', f'{os.getuid()}:{os.getgid()}', root)
    for name in ('framework',):
        shutil.move(str(framework.parent / name), root / name)
    framework = root / 'framework'
    # Only remove optional software on the ephemeral GitHub runner.
    # Never run this cleanup in a workstation or self-hosted environment.
    if data['stage'] in LINUX_GPU:
        run('sudo', 'rm', '-rf', '/usr/share/dotnet', '/usr/local/lib/android',
            '/opt/ghc', '/usr/local/share/boost', '/opt/hostedtoolcache/CodeQL')
    packages = ['autoconf', 'automake', 'libtool', 'build-essential', 'bison', 'flex',
        'curl', 'git', 'cmake', 'ninja-build', 'patchelf', 'pkg-config', 'python3',
        'python3-venv', 'python3-pip', 'python3-mako', 'python3-zstandard',
        'debootstrap', 'util-linux', 'zstd',
        'libarchive-tools', 'jq', 'gcc-aarch64-linux-gnu', 'g++-aarch64-linux-gnu',
        'glslang-tools', 'wayland-protocols', 'libfreetype-dev', 'libexpat1-dev',
        'libwayland-dev', 'libwayland-egl-backend-dev', 'libx11-dev', 'libx11-xcb-dev', 'libxcb1-dev',
        'libegl-dev', 'libgl-dev', 'libgles-dev', 'libglvnd-dev', 'libvulkan-dev',
        'libdrm-dev', 'libxrandr-dev', 'libxfixes-dev', 'libxshmfence-dev',
        'libxxf86vm-dev', 'libxcb-dri3-dev', 'libxcb-present-dev', 'libxcb-sync-dev',
        'libxcb-glx0-dev', 'libxcb-dri2-0-dev', 'libxcb-shm0-dev',
        'libxcb-xfixes0-dev', 'libxcb-randr0-dev', 'libxdamage-dev', 'libxext-dev',
        'libelf-dev', 'libudev-dev', 'libgbm-dev', 'gperf', 'libglib2.0-dev', 'libglib2.0-dev-bin',
        'ccache', 'gettext', 'libgettextpo-dev', 'libssl-dev', 'unzip', 'autopoint', 'intltool', 'python-is-python3']
    if data['stage'] == 'linux-ibus':
        packages = ['build-essential', 'git', 'python3', 'python3-venv', 'ccache', 'pkg-config',
                    'libglib2.0-dev', 'libgtk-3-dev', 'libdbus-1-dev', 'libdconf-dev',
                    'libwayland-dev', 'libxkbcommon-dev', 'wayland-protocols', 'valac',
                    'gobject-introspection', 'libgirepository1.0-dev', 'gettext', 'zstd', 'patchelf',
                    'iso-codes', 'python3-gi', 'autoconf', 'automake', 'libtool', 'autopoint', 'gtk-doc-tools']
    elif data['stage'] == 'guest-tools':
        packages = ['python3', 'python3-venv', 'git', 'zstd', 'ccache']
    elif data['stage'] == 'vulkan-capture':
        # The native capture product does not compile Mesa/libhybris or prepare
        # a distribution. Avoid their autotools, cross toolchain and SDK setup.
        packages = ['build-essential', 'git', 'cmake', 'ninja-build', 'ccache', 'pkg-config',
                    'python3', 'python3-venv', 'libvulkan-dev',
                    'liblz4-dev', 'libzstd-dev', 'zlib1g-dev', 'libxxhash-dev',
                    'libxcb-keysyms1-dev', 'libx11-xcb-dev', 'libxrandr-dev', 'libxrender-dev',
                    'libxext-dev', 'libwayland-dev', 'wayland-protocols']
    run('sudo', 'apt-get', 'update')
    run('sudo', 'apt-get', 'install', '-y', '--no-install-recommends', *packages,
        env=dict(os.environ, DEBIAN_FRONTEND='noninteractive'))
    run('python3', '-m', 'venv', root / 'python')
    python_packages = ['zstandard==0.25.0']
    if data['stage'] not in ('vulkan-capture', 'guest-tools', 'linux-ibus'):
        python_packages += ['meson==1.12.0', 'mako==1.3.10', 'packaging==25.0', 'PyYAML==6.0.2']
    run(root / 'python/bin/pip', 'install', *python_packages)
    bin_dir = root / 'bin'
    bin_dir.mkdir(exist_ok=True)
    cross = bin_dir / 'aarch64-linux-gnu-pkg-config'
    cross.write_text('#!/bin/sh\nexport PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig\nexec pkg-config "$@"\n')
    cross.chmod(0o755)
    if data['stage'] in ('guest-tools', 'linux-ibus'):
        pass  # These recipes require no submodules.
    elif data['stage'] == 'vulkan-capture':
        run('git', '-C', framework, 'submodule', 'update', '--init', '--depth', '1',
            'third_party/gfxreconstruct', 'third_party/libhybris')
        run('git', '-C', framework/'third_party/gfxreconstruct', 'submodule', 'update', '--init', '--depth', '1',
            'external/Vulkan-Headers', 'external/SPIRV-Headers', 'external/SPIRV-Reflect')
    else:
        modules = ['third_party/mesa', 'third_party/libhybris', 'third_party/android-headers']
        if data['stage'] in ROOTFS:
            modules.append('distributions/'+data['stage'])
        run('git', '-C', framework, 'submodule', 'update', '--init', '--depth', '1', *modules)
        if data['stage'] in ROOTFS:
            product = framework/'distributions'/data['stage']
            run('git', '-C', product, 'submodule', 'update', '--init', '--recursive', '--depth', '1')
    nested_products = (framework/'distributions').glob('*/third_party/**/.git')
    for marker in [framework/'.git', *framework.glob('third_party/*/.git'),
                   *framework.glob('distributions/*/.git'), *nested_products]:
        if marker.exists():
            run('git', 'config', '--global', '--add', 'safe.directory', marker.parent)
    with Path(os.environ['GITHUB_PATH']).open('a') as out:
        out.write(str(root / 'python/bin')+'\n'+str(bin_dir)+'\n')
    with Path(os.environ['GITHUB_ENV']).open('a') as out:
        out.write('ARLINUX_FRAMEWORK='+framework.as_posix()+'\n')
        out.write('ARLINUX_WORKSPACE='+root.as_posix()+'\n')
    # Cache immutable stage archives and receipts, never the root-owned scratch
    # rootfs (including /root and ldconfig's private directories). Those trees
    # made Actions' unprivileged tar fail and discarded the entire cache.
    paths = [str(root/'cache/ccache'), str(framework/'build/pipeline'),
             str(framework/'build/linux')]
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as out:
        out.write('cache-paths<<ARLINUX_CACHE_PATHS\n'+'\n'.join(paths)+'\nARLINUX_CACHE_PATHS\n')


def import_dependency(root, dependency, visited=None, *, recursive=True):
    if visited is None:
        visited = set()
    if dependency['identity'] in visited:
        return
    visited.add(dependency['identity'])
    directory = root / 'incoming' / dependency['identity']
    directory.mkdir(parents=True, exist_ok=True)
    manifest = download(dependency['manifestUrl'], directory/'component.json', dependency['manifestSha256'])
    record = json.loads(manifest.read_text())
    if record['identity'] != dependency['identity']:
        raise ValueError('Dependency identity mismatch')
    if recursive:
        for parent in record.get('dependencies', []):
            import_dependency(root, parent, visited)
    for part in record['parts']:
        download(part['url'], directory/part['name'], part['sha256'])
    archive = directory / 'capsule.tar.zst'
    with archive.open('wb') as dest:
        for part in record['parts']:
            with (directory/part['name']).open('rb') as source:
                shutil.copyfileobj(source, dest)
    if sha(archive) != record['archiveSha256']:
        raise ValueError('Reassembled component checksum mismatch')
    def capsule_filter(member, destination):
        original_link = None
        if member.issym() and member.linkname.startswith('/'):
            target = Path(member.linkname)
            if not target.is_relative_to(root):
                raise ValueError('Component link escapes build workspace')
            original_link = member.linkname
            member = member.replace(linkname=os.path.relpath(target, root/Path(member.name).parent))
        filtered = tarfile.data_filter(member, destination)
        if filtered.isfile() or filtered.islnk():
            # Git pack objects are read-only. Restoring over a prior Actions
            # cache must replace the file before writing, then restore its
            # exact original mode for the build-cache receipt to match.
            existing=Path(destination)/filtered.name
            if existing.is_file() or existing.is_symlink():
                existing.unlink()
            filtered=filtered.replace(mode=member.mode & 0o777)
        return filtered.replace(linkname=original_link) if original_link else filtered
    def checked_members(source):
        for member in source:
            if member.name.startswith('/') or '..' in Path(member.name).parts:
                raise ValueError('Unsafe component capsule path')
            yield member
    import zstandard
    with archive.open('rb') as compressed, zstandard.ZstdDecompressor().stream_reader(compressed) as decoded:
        with tarfile.open(fileobj=decoded, mode='r|') as source:
            source.extractall(root, members=checked_members(source), filter=capsule_filter)
    archive.unlink()
    for part in record['parts']:
        (directory/part['name']).unlink()


def restore_seed(root, framework, data, visited):
    if not data.get('cacheSeed'):
        return
    stage_cache = framework/'build/pipeline'/data['stage']
    if any(stage_cache.glob('*/receipt.json')):
        print('Keep restored Actions cache before release seed: '+data['stage'], flush=True)
        return
    # Seed only this component. Current dependencies follow separately; their
    # fixed debug endpoints may already have replaced the seed's old inputs.
    import_dependency(root, data['cacheSeed'], visited, recursive=False)


def build(data, framework):
    root = Path(os.environ['ARLINUX_WORKSPACE'])
    stage = data['stage']
    visited = set()
    restore_seed(root, framework, data, visited)
    for dependency in data['dependencies']:
        import_dependency(root, dependency, visited)
    config = root / 'product.json'
    if stage == 'linux-ibus':
        run('bash', framework/'build.sh', 'input',
            env=dict(os.environ, CCACHE_DIR=str(root/'cache/ccache'), CCACHE_BASEDIR=str(framework)))
    elif stage == 'guest-tools':
        run('bash', framework/'build.sh', 'guest-tools')
    elif stage == 'vulkan-capture':
        run('bash', framework/'build.sh', 'capture',
            env=dict(os.environ, CCACHE_DIR=str(root/'cache/ccache'), CCACHE_BASEDIR=str(framework)))
    elif stage in LINUX_GPU:
        env = dict(os.environ, ARLINUX_CACHE_DIR=str(root/'cache/rootfs'),
                   CCACHE_DIR=str(root/'cache/ccache'),CCACHE_BASEDIR=str(framework))
        run('ccache','--max-size','2G',env=env)
        run('bash', framework/'build.sh', 'gpu', '--component', stage.removeprefix('linux-'), env=env)
    elif stage in ROOTFS:
        config.write_text(json.dumps(dict(format=1, framework=str(framework))))
        output = framework / ('build/artifacts/rootfs/'+stage+'/default.zip')
        run('sudo', '-E', 'env', 'PATH='+os.environ['PATH'], 'HOME='+os.environ['HOME'],
            'ARLINUX_CACHE_DIR='+str(root/'cache/rootfs'),
            'ARLINUX_REQUIRE_GPU_CACHE=1',
            'bash', framework/'build.sh', 'rootfs', '--config', config,
            '--distribution', stage, '--output', output)
        # Only host build-cache files change owner; guest metadata remains in
        # snapshot.tar and the scratch rootfs is left untouched.
        run('sudo', 'chown', '-R', f'{os.getuid()}:{os.getgid()}', framework/'build/pipeline')



def publish(data, framework):
    root = Path(os.environ['ARLINUX_WORKSPACE'])
    stage = data['stage']
    output = root / 'export'
    output.mkdir(exist_ok=True)
    record = dict(data)
    paths = []
    if stage in STANDALONE:
        product = 'guest-tools' if stage == 'guest-tools' else 'capture' if stage == 'vulkan-capture' else 'rootfs/'+stage
        paths = [framework/('build/artifacts/'+product)]
    else:
        sys.path.insert(0, str(framework/'tools/build'))
        from pipeline_cache import inventory, build_references
        descriptor = json.loads((framework/'build/artifacts/components'/f'{stage}.json').read_text())
        state = descriptor['records'][stage]
        path = Path(descriptor['output'])
        if inventory(path) != state['outputs'] or build_references(path) != state['references']:
            raise ValueError('Component output differs from its build-cache receipt')
        paths = [path.parent, path.parent.parent/'receipt.json',
                 *(Path(name) for name in state['references'])]
    # A forced hosted rerun must verify the rebuilt/cached bytes, but need not
    # recompress or upload the same immutable capsule a second time.
    from pipeline_cache import inventory
    contents = ({'outputs': state['outputs'], 'references': state['references']}
                if stage not in STANDALONE else {str(p.relative_to(root)): inventory(p) for p in paths})
    record['outputIdentity'] = hashlib.sha256(json.dumps(contents, sort_keys=True).encode()).hexdigest()
    tag = load_plan(framework/'tools/build')['stages'][stage]['release']
    repository = os.environ['GITHUB_REPOSITORY']
    previous = subprocess.run(['gh','api',f'repos/{repository}/releases/tags/{tag}'], capture_output=True, text=True)
    if not previous.returncode:
        assets = json.loads(previous.stdout)['assets']
        completed = next((a for a in assets if a['name'] == 'component.json'), None)
        if completed:
            expected = completed['digest'].removeprefix('sha256:')
            saved = download(completed['browser_download_url'], output/'previous.json', expected)
            prior = json.loads(saved.read_text())
            if prior['identity'] == data['identity'] and prior.get('outputIdentity') != record['outputIdentity']:
                raise ValueError('Published component differs from the verified rerun outputs')
            if prior['identity'] == data['identity']:
                print('OK identical published component '+stage, flush=True)
                return
    archive = output / 'capsule.tar'
    # Toolchains, pinned checkouts and declared pipeline dependencies are
    # provisioned separately and must not be duplicated into every capsule.
    position = 0
    while position < len(paths):
        path = paths[position]
        position += 1
        for link in ([path] if path.is_symlink() else path.rglob('*') if path.is_dir() else []):
            if not link.is_symlink():
                continue
            # Source checkouts may intentionally contain dangling links (for
            # example Mesa's .github/AGENTS.md). Preserve their link text;
            # they do not introduce an additional build reference.
            target = link.resolve(strict=False)
            if not target.is_relative_to(root):
                raise ValueError('Build reference is outside the component workspace')
            if not target.exists():
                continue
            if any(target == parent or target.is_relative_to(parent) for parent in paths):
                continue
            if any(target.is_relative_to(parent) for parent in (
                    root/'toolchains', framework/'third_party', framework/'build/pipeline')):
                continue
            paths.append(target)
    with tarfile.open(archive, 'w') as target:
        seen = set()
        for path in paths:
            relative = path.resolve().relative_to(root.resolve())
            if relative in seen:
                continue
            seen.add(relative)
            target.add(path, arcname=relative.as_posix())
    compressed = archive.with_suffix('.tar.zst')
    import zstandard
    with archive.open('rb') as source, compressed.open('wb') as dest:
        with zstandard.ZstdCompressor(level=3, threads=4).stream_writer(dest) as writer:
            shutil.copyfileobj(source, writer)
    archive.unlink()
    record['archiveSha256'] = sha(compressed)
    tag = load_plan(framework/'tools/build')['stages'][stage]['release']
    repository = os.environ['GITHUB_REPOSITORY']
    record['parts'] = []
    with compressed.open('rb') as source:
        while block := source.read(1024*1024*1024):
            name = f'capsule-{record["archiveSha256"]}-{len(record["parts"]):03d}.zst.part'
            part = output/name
            part.write_bytes(block)
            record['parts'].append(dict(name=name, sha256=sha(part),
                url=f'https://github.com/{repository}/releases/download/{tag}/{name}'))
    manifest = output/'component.json'
    manifest.write_text(json.dumps(record,sort_keys=True,indent=2)+'\n')
    # Each component owns one fixed release. Upload verified payloads first;
    # component.json is the final completion marker for this generation.
    existing = subprocess.run(['gh','api',f'repos/{repository}/releases/tags/{tag}'],
                              capture_output=True,text=True)
    if existing.returncode:
        run('gh','release','create',tag,'--repo',repository,'--prerelease','--title',tag,
            '--notes','Verified ARLinux build component; not an application release.')
        assets = {}
    else:
        assets = {asset['name']:asset for asset in json.loads(existing.stdout)['assets']}
    # Payload names contain their archive digest so an in-flight reader cannot
    # mix generations. Replace the completion marker only after all uploads.
    for file in [*(output/part['name'] for part in record['parts']), manifest]:
        if file.name in assets:
            if assets[file.name].get('digest') != 'sha256:'+sha(file):
                run('gh','release','upload',tag,'--repo',repository,file,'--clobber')
        else:
            run('gh','release','upload',tag,'--repo',repository,file)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare','build','publish'))
    args = parser.parse_args()
    data, framework = request()
    try:
        globals()[args.action](data, framework)
    except subprocess.CalledProcessError:
        # Existing recipes retain detailed compiler logs in their work trees.
        # Surface them when a hosted build fails so diagnosis needs no SSH.
        candidates=[p for p in (framework/'build').rglob('*.log') if p.is_file()]
        for path in sorted(candidates,key=lambda p:p.stat().st_mtime,reverse=True)[:8]:
            print('::group::'+str(path),flush=True)
            print('\n'.join(path.read_text(errors='replace').splitlines()[-60:]),flush=True)
            print('::endgroup::',flush=True)
        raise
