"""Content identities for independently dispatched distribution components."""
import hashlib
import json
import subprocess


def source_inputs(repo, revision, stage, plan):
    distribution = stage if stage in ('yibu', 'debian', 'arch', 'omarchy', 'plasma', 'lxqt') else 'yibu'
    if stage == 'guest-tools':
        paths = ('build.sh', 'tools/build/github-component.py', 'tools/build/component_inputs.py',
                 'tools/build/guest-tools.py', 'tools/build/pipeline_cache.py', 'runtime/tools',
                 'tools/arlinux-app-data/hosted-ime.py')
    elif stage == 'linux-ibus':
        paths = ('build.sh', 'tools/build/github-component.py', 'tools/build/component_inputs.py',
                 'tools/build/ibus-component.py', 'tools/build/pipeline_cache.py')
    elif stage == 'vulkan-capture':
        paths = ('build.sh', 'tools/build/github-component.py', 'tools/build/component_inputs.py',
                 'tools/build/vulkan-capture-component.py', 'tools/build/pipeline_cache.py',
                 'third_party/gfxreconstruct', 'third_party/libhybris')
    elif stage in ('linux-mesa', 'linux-hybris'):
        paths = ('build.sh', 'tools/build/github-component.py', 'tools/build/component_inputs.py',
                 'tools/build/linux-gpu.sh', 'tools/build/linux-gpu-component.py',
                 'tools/build/linux-aarch64.ini', 'tools/build/pipeline_cache.py', 'third_party')
    else:
        paths = ('build.sh', 'tools', 'runtime', 'examples', 'third_party', 'distributions/'+distribution)
    records = subprocess.check_output(['git', '-C', str(repo), 'ls-tree', '-r', '-z', revision, '--', *paths])
    files = {}
    for record in records.split(b'\0'):
        if record:
            metadata, name = record.split(b'\t', 1)
            name = name.decode()
            if stage in ('yibu', 'debian', 'arch', 'omarchy', 'plasma', 'lxqt'):
                if name.startswith('runtime/tools/') and name.rsplit('/', 1)[-1] not in (
                        'guest-sudo.c', 'bwrap-compat.c', 'bwrap-view.h', 'install-mesa-arch.sh'):
                    continue
                if name in ('tools/build/guest-tools.py', 'tools/arlinux-app-data/hosted-ime.py'):
                    continue
            # Workstation transport does not generate distribution contents.
            # Its downloads still verify the declared component/output hashes.
            if name in ('tools/build/github-products.py', 'tools/build/artifact_download.py'):
                continue
            files[name] = metadata.decode()
    return dict(sourceSha256=hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(),
                runner=plan['stages'][stage]['runner'])


def request_identity(data):
    fields = {k: v for k, v in data.items() if k not in ('identity', 'cacheSeed')}
    if data.get('format') == 2:
        fields.pop('framework')
    return hashlib.sha256(json.dumps(fields, sort_keys=True).encode()).hexdigest()


def package_inputs(common):
    """Package preparation does not execute application Python or vendor downloads.

    seal-rootfs refreshes those files from the current seed after restoring the
    package snapshot. Keep installer shell scripts and native helper sources.
    """
    sources = dict(common['sources'])
    # These are compiled helpers and the package installer, not application
    # entry points. Extensionless Python/shell launchers are runtime-only too.
    package_runtime = {'guest-sudo.c', 'bwrap-compat.c', 'bwrap-view.h', 'install-mesa-arch.sh'}
    sources['runtime/tools'] = {name: value for name, value in sources['runtime/tools'].items()
                               if name in package_runtime}
    sources.pop('tools/build/install-capture.py', None)
    # Capture tools are refreshed from the verified current seed by seal-rootfs;
    # package preparation neither executes them nor installs their dependencies.
    return {name: value for name, value in dict(common, sources=sources).items()
            if name != 'capture'}
