#!/usr/bin/env python3
"""Package first-party application integration independently of the Linux image."""
import hashlib
import json
from pathlib import Path
import stat
import zipfile


def main():
    framework = Path(__file__).resolve().parents[2]
    source = framework/'runtime/tools'
    files = {}

    def add(path, target, mode=0o644):
        data = path.read_bytes()
        files[target] = (data, mode)

    for path in sorted(source.glob('*.py')):
        target = 'usr/lib/arlinux/guest/yibu-launcher.py' if path.name == 'apps.py' else 'usr/lib/arlinux/'+path.name
        add(path, target)
    for path in sorted((source/'guest').rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            add(path, 'usr/lib/arlinux/guest/'+path.relative_to(source/'guest').as_posix())
    for path in sorted((source/'python').rglob('*.py')):
        add(path, 'usr/lib/arlinux/python/'+path.relative_to(source/'python').as_posix())
    for path in sorted(source.glob('*.tsv')):
        add(path, 'usr/lib/arlinux/guest/'+path.name)
    for path in sorted(source.glob('*.desktop')):
        add(path, 'usr/share/applications/'+path.name)
    add(source/'browser-mimeapps.list', 'etc/xdg/mimeapps.list')
    for path in sorted([*source.glob('*.png'), *source.glob('*.svg')]):
        add(path, 'usr/share/pixmaps/arlinux-'+path.name)
    for path in sorted(source.glob('arlinux-*')):
        if path.is_file() and '.' not in path.name:
            add(path, 'usr/bin/'+path.name, 0o755)
    add(framework/'tools/arlinux-app-data/hosted-ime.py', 'usr/lib/arlinux/hosted-ime.py')
    manifest = dict(format=1, product='arlinux-guest-tools', api=1,
                    files={name: dict(sha256=hashlib.sha256(data).hexdigest(), mode=mode)
                           for name, (data, mode) in files.items()})
    output = framework/'build/artifacts/guest-tools'
    output.mkdir(parents=True, exist_ok=True)
    archive = output/'guest-tools.zip'
    temporary = archive.with_suffix('.zip.part')
    with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as target:
        for name, data, mode in [('manifest.json', (json.dumps(manifest, sort_keys=True)+'\n').encode(), 0o644),
                                 *((name, data, mode) for name, (data, mode) in files.items())]:
            entry = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = (stat.S_IFREG | mode) << 16
            target.writestr(entry, data)
    temporary.replace(archive)
    receipt = dict(format=1, product='arlinux-guest-tools', api=1,
                   archiveSha256=hashlib.sha256(archive.read_bytes()).hexdigest())
    (output/'guest-tools.json').write_text(json.dumps(receipt, sort_keys=True)+'\n')
    print('ARLinux Guest Tools ready: '+str(archive), flush=True)


if __name__ == '__main__':
    main()
