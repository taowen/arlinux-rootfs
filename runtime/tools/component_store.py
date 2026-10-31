"""Verified GitHub release components, independent of product selection policy."""
from html.parser import HTMLParser
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import time

import shared


def release_asset(row):
    name, repository, tag, filename = row
    if (not re.fullmatch(r'[a-z0-9_-]+', name)
            or not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository)
            or not re.fullmatch(r'[A-Za-z0-9_.-]+', tag)
            or not re.fullmatch(r'[A-Za-z0-9_.-]+', filename)):
        raise ValueError('Invalid component release endpoint')
    url = f'https://github.com/{repository}/releases/download/{tag}/{filename}'
    try:
        release = json.loads(shared.github_metadata(f'https://api.github.com/repos/{repository}/releases/tags/{tag}'))
    except (OSError, subprocess.SubprocessError, ValueError):
        class Assets(HTMLParser):
            def __init__(self):
                super().__init__()
                self.checksums, self.link = [], False

            def handle_starttag(self, tag_name, attrs):
                attrs = dict(attrs)
                if tag_name == 'a' and attrs.get('href') == f'/{repository}/releases/download/{tag}/{filename}':
                    self.link = True
                if tag_name == 'clipboard-copy' and attrs.get('aria-label') == 'Copy to clipboard digest for '+filename:
                    self.checksums.append(attrs.get('value', ''))

        assets = Assets()
        assets.feed(shared.github_metadata(f'https://github.com/{repository}/releases/expanded_assets/{tag}').decode('utf-8'))
        if not assets.link or len(assets.checksums) != 1 or not re.fullmatch('sha256:[0-9a-f]{64}', assets.checksums[0]):
            raise ValueError('Component release has no verified digest: '+name)
        checksum, size = assets.checksums[0][7:], None
    else:
        if release.get('draft') or release.get('tag_name') != tag:
            raise ValueError('Invalid component release')
        asset = next((a for a in release.get('assets', []) if a.get('name') == filename), None)
        if (not asset or asset.get('state') != 'uploaded' or asset.get('browser_download_url') != url
                or not isinstance(asset.get('digest'), str) or not re.fullmatch('sha256:[0-9a-f]{64}', asset['digest'])
                or type(asset.get('size')) is not int or asset['size'] <= 0):
            raise ValueError('Component release has no verified asset: '+name)
        checksum, size = asset['digest'][7:], asset['size']
    return dict(name=name, file=filename, asset=filename, sha256=checksum, bytes=size, url=url)


class ComponentStore:
    """Download candidates and public archives through one installation path."""
    def __init__(self, root, state, overrides, metadata_name):
        self.root, self.state = Path(root), Path(state)
        self.overrides = Path(overrides) if overrides else None
        self.metadata_name = metadata_name

    def get(self, row):
        name, repository, tag, filename = row
        if not re.fullmatch(r'[a-z0-9_-]+', name):
            raise ValueError('Invalid component name')
        override = self.overrides/(name+'.json') if self.overrides else None
        state = self.state/(name+'.json')
        if override is not None and override.is_file():
            checksum = json.loads(override.read_text())['sha256']
            if not re.fullmatch('[0-9a-f]{64}', checksum):
                raise ValueError('Invalid component descriptor checksum')
            source = shared.directory('downloads')/(checksum+'.json')
            if shared.digest(source) != checksum:
                raise ValueError('Component descriptor checksum mismatch')
            descriptor = json.loads(source.read_text())
        else:
            try:
                cached = json.loads(state.read_text())
                age = time.time()-cached['checkedAt']
                if (0 <= age < 3600 and cached['endpoint'] == row
                        and re.fullmatch('[0-9a-f]{64}', cached['cache'])):
                    destination = self.root/name/cached['cache']
                    if destination.is_dir():
                        return destination
            except (FileNotFoundError, ValueError, KeyError, TypeError):
                pass
            descriptor = release_asset(row)
            encoded = (json.dumps(descriptor, sort_keys=True)+'\n').encode()
            checksum = hashlib.sha256(encoded).hexdigest()
        identity = descriptor['sha256'] if descriptor.get('name') == name else checksum
        if not re.fullmatch('[0-9a-f]{64}', identity):
            raise ValueError('Invalid component archive identity')
        home = self.root/name
        home.mkdir(parents=True, exist_ok=True)
        destination = home/identity
        with shared.locked(home/'install.lock'):
            if not destination.is_dir():
                if descriptor.get('kind') == 'arlinux-bionic-components' and descriptor.get('format') == 1:
                    archives, metadata = descriptor['components'], descriptor['runtime']
                elif descriptor.get('name') == name:
                    archives, metadata = [descriptor], None
                else:
                    raise ValueError('Invalid component descriptor: '+name)
                with tempfile.TemporaryDirectory(prefix='install-', dir=home) as temporary:
                    stage = Path(temporary)/'component'
                    stage.mkdir()
                    seen = set()
                    for item in archives:
                        asset = item.get('asset', item['file'])
                        if (not re.fullmatch('[0-9a-f]{64}', item['sha256']) or not re.fullmatch(
                                r'https://github[.]com/'+re.escape(repository)+r'/releases/download/[^/]+/'+re.escape(asset), item['url'])):
                            raise ValueError('Invalid component archive')
                        print('Preparing runtime component: '+name, flush=True)
                        archive = shared.download(item['url'], item['sha256'], '.tar.gz', progress=False)
                        if item.get('bytes') is not None and archive.stat().st_size != item['bytes']:
                            raise ValueError('Component archive size mismatch')
                        with tarfile.open(archive, 'r:*') as bundle:
                            for member in bundle:
                                path = member.name
                                if path == self.metadata_name:
                                    if metadata is not None or not member.isfile() or member.size > 4*1024*1024:
                                        raise ValueError('Invalid embedded component metadata')
                                    metadata = json.load(bundle.extractfile(member))
                                    continue
                                if (path in seen or '\\' in path or ':' in path or member.isdir()
                                        or any(part in ('', '.', '..') for part in path.split('/'))):
                                    raise ValueError('Unsafe or duplicate component path: '+path)
                                seen.add(path)
                                bundle.extract(member, stage, filter='data')
                    if not isinstance(metadata, dict):
                        raise ValueError('Missing component metadata')
                    (stage/self.metadata_name).write_text(json.dumps(metadata)+'\n')
                    self.validate(stage)
                    stage.rename(destination)
        if override is None or not override.is_file():
            state.parent.mkdir(parents=True, exist_ok=True)
            with shared.locked(state.with_suffix('.lock')):
                pending = state.with_suffix('.tmp')
                pending.write_text(json.dumps(dict(cache=identity, endpoint=row, checkedAt=time.time()))+'\n')
                pending.replace(state)
        return destination

    def validate(self, root):
        root = root.resolve()
        metadata = json.loads((root/self.metadata_name).read_text())
        for relative, checksum in metadata['files'].items():
            path = root/relative
            if (not path.resolve().is_relative_to(root) or not re.fullmatch('[0-9a-f]{64}', checksum)
                    or shared.digest(path) != checksum):
                raise ValueError('Component file checksum mismatch: '+relative)
        return metadata
