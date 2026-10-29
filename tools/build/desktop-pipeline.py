#!/usr/bin/env python3
"""Build the Linux rootfs independently of Android NDK and Bionic Wine builds."""
import argparse
import fcntl
import hashlib
import inspect
import json
import os
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pipeline_cache import Cache, digest, inventory, run, source_state
from component_inputs import package_inputs


def rootfs_sources(framework, product):
    inputs = {name: inventory(framework / name) for name in (
        'runtime/tools', 'tools/arlinux-app-data', 'examples', 'third_party/libhybris/arlinux/graphics-protocols')}
    inputs['runtime/tools'] = {name: value for name, value in inputs['runtime/tools'].items()
                              if name in ('guest-sudo.c', 'bwrap-compat.c', 'bwrap-view.h', 'install-mesa-arch.sh')}
    inputs['tools/arlinux-app-data'].pop('hosted-ime.py', None)
    for name in ('third_party/mesa', 'third_party/libhybris',
                 'third_party/android-headers', 'distributions/' + product):
        inputs[name] = source_state(framework / name)
    for name in ('linux-assets.sh', 'linux-gpu.sh', 'linux-aarch64.ini',
                 'mesa-debs.py', 'mesa-arch.py', 'doctor.sh',
                 'package-rootfs.py', 'package-rootfs.sh', 'pipeline_cache.py', 'component_inputs.py', 'install-capture.py'):
        inputs['tools/build/' + name] = inventory(framework / 'tools/build' / name)
    return inputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--distribution', default='yibu', choices=('yibu', 'debian', 'arch', 'omarchy', 'plasma', 'lxqt'))
    parser.add_argument('--target', choices=('linux-assets', 'rootfs-packages', 'desktop'), default='desktop')
    parser.add_argument('--rebuild', action='store_true')
    args = parser.parse_args()
    product = args.distribution
    config = json.loads(args.config.read_text())
    if config.get('format') != 1 or set(config) != {'format', 'framework'}:
        raise ValueError('Rootfs config accepts only format and framework; Wine toolchains belong to wine-build.json')
    framework = Path(config['framework']).resolve(strict=True)
    env = {name: value for name, value in os.environ.items() if name in (
        'PATH', 'HOME', 'LANG', 'LC_ALL', 'TMPDIR', 'SSL_CERT_FILE', 'SSL_CERT_DIR',
        'http_proxy', 'https_proxy', 'no_proxy', 'ARLINUX_CACHE_DIR')}
    cache_root = framework / 'build/pipeline'
    cache_root.mkdir(parents=True, exist_ok=True)
    with (cache_root / '.rootfs.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        cache = Cache(cache_root, args.rebuild)
        invoke = lambda *command: run(*command, cwd=framework, env=env)
        if os.environ.get('ARLINUX_REQUIRE_GPU_CACHE'):
            env['ARLINUX_REQUIRE_GPU_CACHE'] = os.environ['ARLINUX_REQUIRE_GPU_CACHE']
        invoke('bash', 'tools/build/linux-gpu.sh')
        gpu = {name: json.loads((framework / 'build/linux' / name).read_text())
               for name in ('gpu.mesa.outputs.json', 'gpu.hybris.outputs.json')}
        common = dict(engine=digest(__file__), sources=rootfs_sources(framework, product),
                      gpu=gpu, distribution=product,
                      capture=digest(framework/'build/artifacts/capture/capture.zip'),
                      input=digest(framework/'build/linux/ibus.outputs.json'))
        def assets(out):
            invoke('bash', 'tools/build/linux-assets.sh', *(['--rebuild'] if args.rebuild else []), product)
            shutil.copytree(framework / 'distributions' / product / 'build/assets', out, symlinks=True)
        assets = cache.stage('linux-assets', common, assets)
        if args.target == 'linux-assets':
            print('Completed Linux assets', flush=True)
            return
        target = framework / 'distributions' / product / 'build/assets'
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(assets, target, symlinks=True)
        def packages(out):
            out.mkdir()
            invoke('python3', 'tools/bundle.py', 'pack', 'distributions/' + product, out / 'seed.zip')
            invoke('python3', 'tools/build/package-rootfs.py', out / 'seed.zip', out / 'packages')
            (out / 'seed.zip').unlink()
        package_engine = hashlib.sha256(inspect.getsource(packages).encode()).hexdigest()
        packages = cache.stage('rootfs-packages', dict(source=package_inputs(dict(common, engine=package_engine)),
            recipe={name: inventory(framework / name) for name in ('tools/bundle.py', 'tools/build/package-rootfs.py', 'tools/build/package-rootfs.sh')},
            executor='native-aarch64'), packages)
        if args.target == 'rootfs-packages':
            print('Completed Linux packages', flush=True)
            return
        snapshot = packages / 'packages/snapshot.tar'
        inputs = dict(common, dependencies=cache.records.copy(), snapshot=digest(snapshot),
                      recipe={name: inventory(framework / name) for name in ('tools/seal-rootfs.py', 'tools/bundle.py')})
        def desktop(out):
            out.mkdir()
            invoke('python3', 'tools/bundle.py', 'pack', 'distributions/' + product, out / 'seed.zip')
            invoke('python3', 'tools/seal-rootfs.py', 'seal', out / 'seed.zip', snapshot, out / 'offline.zip')
            shutil.copyfile(out / 'offline.zip', out / 'default.zip')
            (out / 'seed.zip').unlink()
            (out / 'offline.zip').unlink()
        result = cache.stage('desktop', inputs, desktop)
        publish_result(args, config, cache, result)


def publish_result(args, config, cache, result):
    args.output.parent.mkdir(parents=True, exist_ok=True)
    pending = args.output.with_suffix('.zip.part')
    shutil.copyfile(result / 'default.zip', pending)
    pending.replace(args.output)
    receipt = dict(format=1, desktopSha256=digest(args.output), configSha256=digest(args.config), stages=cache.records)
    draft = args.output.with_suffix('.json.part')
    draft.write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
    draft.replace(args.output.with_suffix('.json'))
    print('Linux rootfs ready: ' + str(args.output), flush=True)


if __name__ == '__main__':
    main()
