"""App-owned cross-instance downloads; never place accounts or prefixes here.

The Android host exposes files/arlinux-shared at /arlinux-shared, independently
of instance selection, deletion and reinstall.
There is deliberately no per-HOME fallback: an old host must be upgraded.
"""
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
from pathlib import Path
import re
import subprocess
import shutil
import time
import tempfile
import sys
from urllib.parse import urlparse, unquote

ROOT = Path('/arlinux-shared')
CURL_TLS = ['--proto', '=https', '--proto-redir', '=https']


def directory(name):
    if not ROOT.is_dir():
        raise RuntimeError('Update ARLinux: the host must provide /arlinux-shared')
    if not re.fullmatch(r'[a-z0-9][a-z0-9/-]*', name) or '..' in name:
        raise ValueError('Invalid shared directory')
    path = ROOT / name
    path.mkdir(parents=True, exist_ok=True)
    return path


@contextmanager
def locked(path):
    with path.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def network_policy():
    """Android owns locale and routes; a missing policy means ordinary direct networking."""
    try:
        policy = json.loads(Path('/etc/arlinux-network.json').read_text())
        return policy if isinstance(policy, dict) and policy.get('enabled') is True else {}
    except (OSError, ValueError):
        return {}


def acceleration_enabled():
    return bool(network_policy())


def github_asset(url, expected):
    """Use GitHub's official asset API when its web download host is unreachable."""
    parsed = urlparse(url)
    match = re.fullmatch(r'/([^/]+)/([^/]+)/releases/download/([^/]+)/([^/]+)', parsed.path)
    if parsed.scheme != 'https' or parsed.netloc != 'github.com' or not match:
        return None
    owner, repo, tag, filename = match.groups()
    endpoint = f'https://api.github.com/repos/{owner}/{repo}/releases/tags/{tag}'

    try:
        release = json.loads(github_metadata(endpoint))
        for asset in release.get('assets', []):
            if asset.get('name') != unquote(filename):
                continue
            identity = asset.get('id')
            if not isinstance(identity, int) or identity <= 0:
                return None
            checksum = asset.get('digest')
            if expected is not None and checksum is not None and checksum != 'sha256:' + expected:
                return None
            return f'https://api.github.com/repos/{owner}/{repo}/releases/assets/{identity}'
    except (OSError, subprocess.SubprocessError, ValueError, AttributeError):
        pass
    return None


def ranked_sources(urls):
    """Compare bounded real transfers across official mirrors, never log signed URLs."""
    if len(urls) < 2 or not acceleration_enabled():
        return urls

    def probe(url):
        try:
            result = subprocess.run(['curl', *CURL_TLS, '-fsSL', '--connect-timeout', '2', '--max-time', '12',
                '--range', '0-1048575', '--output', '/dev/null', '--write-out',
                '%{http_code} %{size_download} %{speed_download}', url],
                capture_output=True, text=True, timeout=14)
            status, size, speed = result.stdout.strip().split()
            # curl 28 is expected if a server ignores Range and streams the whole file.
            if result.returncode in (0, 28) and status in ('200', '206') and float(size) >= 16384:
                return float(speed)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            pass
        return 0

    with ThreadPoolExecutor(max_workers=3) as pool:
        rates = list(pool.map(probe, urls))
    ordered = sorted(zip(urls, rates), key=lambda item: -item[1])
    if ordered[0][1]:
        print('Selected official download CDN: ' + urlparse(ordered[0][0]).hostname,
              flush=True, file=sys.stderr)
    return [url for url, speed in ordered]



def download_route_options(url, headers):
    """Consume host-selected CDN routes without changing HTTP Host or TLS trust."""
    policy = network_policy()
    host = urlparse(url).hostname
    if not policy:
        return [headers]
    routes = [headers]
    for destinations in policy.get('githubRoutes', []):
        if host not in destinations:
            continue
        options = [*headers, '--noproxy', ','.join(destinations)]
        for name, edge in destinations.items():
            options.extend(('--connect-to', f'{name}:443:{edge}:443'))
        routes.append(options)
    for address in policy.get('cdnRoutes', {}).get(host, []):
        routes.append([*headers, '--noproxy', host, '--resolve', f'{host}:443:{address}'])
    return routes


def github_history(key=None, rate=0, success=False):
    """Best-effort transport hints, independent of archive identities or trust."""
    path = directory('downloads')/'github-route-history.json'
    try:
        with locked(path.with_suffix('.lock')):
            try:
                history = json.loads(path.read_text())
                if not isinstance(history, dict):
                    history = {}
                history = {name: row for name, row in history.items()
                           if isinstance(row, dict) and
                           all(isinstance(row.get(field), (int, float)) and 0 <= row[field] < 1e18
                               for field in ('rate', 'updated', 'failures'))}
            except (OSError, ValueError):
                history = {}
            if key is not None:
                old = history.get(key, {})
                previous = max(0, float(old.get('rate', 0)))
                history[key] = dict(rate=round(previous * .5 + max(0, rate) * .5) if previous else round(max(0, rate)),
                                    failures=0 if success else int(old.get('failures', 0))+1,
                                    verified=success, updated=time.time())
                temporary = path.with_suffix('.tmp')
                temporary.write_text(json.dumps(history, sort_keys=True)+'\n')
                temporary.replace(path)
            return history
    except (OSError, ValueError, TypeError, AttributeError):
        return {}


def download_transfer(sources, partial, expected, *, limit=None):
    """Keep the winning live connection instead of reconnecting after a probe."""
    if expected is None and limit is None:
        raise ValueError('Metadata transfer requires a size limit')
    routes = [(url, options, number) for url, headers in sources
              for number, options in enumerate(download_route_options(url, headers))]
    # Public Release payloads only. Metadata and authenticated API requests never
    # go to a third party, and all proxy payloads require an official SHA-256.
    proxy = network_policy().get('releaseProxy')
    if expected is not None and proxy:
        for url, headers in sources:
            parsed = urlparse(url)
            if parsed.scheme == 'https' and parsed.netloc == 'github.com' and re.fullmatch(
                    r'/[^/]+/[^/]+/releases/download/[^/]+/[^/]+', parsed.path):
                routes.append((proxy + url, [*headers, '--noproxy', '*'], 5))
    if len(routes) == len(sources):
        return False

    def response_mode(header, offset):
        try:
            blocks = re.split(rb'\r?\n\r?\n', header.read_bytes())
            block = next(block for block in reversed(blocks) if block.startswith(b'HTTP/'))
            lines = block.splitlines()
            status = lines[0].split()[1]
            if status == b'200' and not offset:
                return 'replace'
            if status == b'206':
                fields = dict(line.lower().split(b':', 1) for line in lines[1:] if b':' in line)
                value = fields.get(b'content-range', b'').strip()
                match = re.fullmatch(rb'bytes ([0-9]+)-([0-9]+)/([0-9]+)', value)
                if match and int(match[1]) == offset and offset <= int(match[2]) < int(match[3]):
                    return 'append'
        except (OSError, StopIteration, IndexError):
            pass
        return None

    def stop(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
        process.wait()

    remaining = list(enumerate(routes))
    while remaining:
        # Release metadata is mutable; retry a complete response instead of
        # combining byte ranges from different snapshots of its JSON/HTML.
        if limit is not None:
            partial.unlink(missing_ok=True)
        offset = partial.stat().st_size if partial.is_file() else 0
        def key(route):
            return f'{urlparse(route[0]).hostname}{"/metadata" if limit else ""}/{"resume" if offset else "full"}/{route[2]}'
        history = github_history()
        def weight(item):
            row = history.get(key(item[1]), {})
            age = max(0, time.time()-row.get('updated', 0))
            return row.get('rate', 0)/(1+row.get('failures', 0))/(1+age/86400)
        remaining.sort(key=weight, reverse=True)
        batch = remaining
        workers = []
        try:
            started = time.monotonic()
            for index, (url, options, _) in batch:
                body = partial.with_name(partial.name + f'.route-{index}')
                header = body.with_name(body.name + '.headers')
                body.unlink(missing_ok=True)
                header.unlink(missing_ok=True)
                # Each candidate holds only its suffix; no copy of the existing
                # download is needed. Redirect headers remain private and temporary.
                process = subprocess.Popen(['curl', *CURL_TLS, '-fsSL', '--connect-timeout', '5',
                    '--speed-limit', '1024', '--speed-time', '30', '--max-redirs', '10',
                    *(['--range', f'{offset}-'] if offset else []),
                    *(['--max-filesize', str(limit), '--max-time', '10'] if limit else []), *options,
                    '--dump-header', str(header), '--output', str(body), url],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                workers.append((index, process, body, header))
            eligible = []
            while time.monotonic() - started < 15:
                eligible = [worker for worker in workers if worker[2].is_file()
                            and worker[2].stat().st_size and response_mode(worker[3], offset)]
                complete = [worker for worker in eligible if worker[1].poll() == 0]
                if complete or eligible and (len(eligible) == len(workers) or time.monotonic() - started >= 8):
                    if complete:
                        eligible = complete
                    break
                if all(worker[1].poll() is not None for worker in workers):
                    break
                time.sleep(.1)
            if not eligible:
                for _, route in batch:
                    github_history(key(route))
                rejected = {index for index, _ in batch}
                remaining = [(i, route) for i, route in remaining if i not in rejected]
                continue
            winner = max(eligible, key=lambda worker: worker[2].stat().st_size)
            index, process, body, header = winner
            rate = body.stat().st_size / max(.1, time.monotonic() - started)
            print(f'Selected live download route {index}',
                  flush=True, file=sys.stderr)
            for worker in workers:
                if worker is not winner:
                    stop(worker[1])
            minimum = 0 if limit or len(remaining) == 1 else max(65536, min(262144, int(rate / 4)))
            window_at, window_bytes = time.monotonic(), body.stat().st_size
            last_progress = window_at
            while process.poll() is None:
                time.sleep(.25)
                now, size = time.monotonic(), body.stat().st_size
                if now - last_progress >= 5:
                    total = size + offset if response_mode(header, offset) == 'append' else size
                    print(f'Download: {total / 1048576:.1f} MiB received',
                          flush=True, file=sys.stderr)
                    last_progress = now
                if now - window_at >= 15:
                    if (size - window_bytes) / (now - window_at) < minimum:
                        stop(process)
                        break
                    window_at, window_bytes = now, size
            mode = response_mode(header, offset)
            if mode == 'append':
                with partial.open('ab') as output, body.open('rb') as source:
                    shutil.copyfileobj(source, output)
            elif mode == 'replace':
                body.replace(partial)
            success = process.returncode == 0 and mode and (
                0 < partial.stat().st_size <= limit if expected is None else digest(partial) == expected)
            route = next(route for i, route in remaining if i == index)
            github_history(key(route), (partial.stat().st_size-offset)/max(.1, time.monotonic()-started), bool(success))
            if success:
                return True
            if process.returncode == 0:
                partial.unlink(missing_ok=True)
            remaining = [(i, route) for i, route in remaining if i != index]
            print('Connection slowed or failed; resuming verified download',
                  flush=True, file=sys.stderr)
        finally:
            for _, process, body, header in workers:
                stop(process)
                body.unlink(missing_ok=True)
                header.unlink(missing_ok=True)
    return False


def github_metadata(url):
    """Fetch bounded official metadata through the same measured route policy."""
    if urlparse(url).scheme != 'https' or urlparse(url).hostname not in ('api.github.com', 'github.com'):
        raise ValueError('Expected official GitHub metadata URL')
    try:
        return download_metadata(url)
    except (OSError, subprocess.SubprocessError):
        # Release metadata is also available through GitHub's official asset
        # API. Do not route integrity metadata through a public release proxy.
        asset = github_asset(url, None)
        if asset is None:
            raise
        return download_metadata(asset, headers=['-H', 'Accept: application/octet-stream'])


def download_metadata(url, limit=2 * 1024 * 1024, *, headers=()):
    """Bounded official metadata; CDN selection never sends it to a release proxy."""
    if urlparse(url).scheme != 'https':
        raise ValueError('Expected HTTPS metadata URL')
    with tempfile.TemporaryDirectory(prefix='.download-metadata-', dir=directory('downloads')) as temporary:
        target = Path(temporary)/'response'
        if not download_transfer([(url, list(headers))], target, None, limit=limit):
            subprocess.run(['curl', *CURL_TLS, *headers, '-fsSL', '--connect-timeout', '3', '--max-time', '10',
                            '--max-filesize', str(limit), '-o', str(target), url],
                           check=True, timeout=12, stdout=sys.stderr)
        data = target.read_bytes()
        if not 0 < len(data) <= limit:
            raise ValueError('Download metadata exceeds size limit')
        return data


def download(url, expected, suffix, *, progress=True):
    """Keep one verified vendor artifact, with a cross-instance download lock."""
    urls = [url] if isinstance(url, str) else list(url)
    if (not re.fullmatch(r'[0-9a-f]{64}', expected)
            or not re.fullmatch(r'\.[a-z0-9.]+', suffix)
            or not urls or any(not u.startswith('https://') for u in urls)):
        raise ValueError('Invalid shared download')
    target = directory('downloads') / (expected + suffix)
    with locked(target.with_suffix(target.suffix + '.lock')):
        if target.is_file() and digest(target) == expected:
            return target
        partial = target.with_suffix(target.suffix + '.part')
        if partial.is_file() and digest(partial) == expected:
            partial.replace(target)
            return target
        def candidates():
            for source in ranked_sources(urls):
                if re.fullmatch(r'https://api\.github\.com/repos/[^/]+/[^/]+/releases/assets/[1-9][0-9]*', source):
                    sources = [(source, ['-H', 'Accept: application/octet-stream'])]
                else:
                    api = github_asset(source, expected)
                    sources = ([(api, ['-H', 'Accept: application/octet-stream'])] if api else []) + [(source, [])]
                yield sources
        failure = None
        for sources in candidates():
            if download_transfer(sources, partial, expected):
                partial.replace(target)
                return target
            for source, headers in sources:
                try:
                    print('Downloading verified installer…', flush=True, file=sys.stderr)
                    for attempt in range(2):
                        try:
                            subprocess.run(['curl', *CURL_TLS, '-fL', *([] if progress else ['-sS']), '--retry', '2', '--connect-timeout', '10',
                                            '--speed-limit', '1024', '--speed-time', '30',
                                            '--continue-at', '-', *headers, '-o', str(partial), source],
                                           check=True, stdout=sys.stderr)
                            break
                        except subprocess.CalledProcessError as error:
                            # Some official mirrors do not support byte ranges.
                            if attempt == 0 and error.returncode in (33, 36):
                                partial.unlink(missing_ok=True)
                                continue
                            if attempt == 0 and error.returncode in (18, 28, 52, 55, 56):
                                # A transient disconnect does not mean the fastest CDN is down.
                                continue
                            raise
                    if digest(partial) != expected:
                        # Never append a different CDN's bytes to a corrupt response.
                        partial.unlink(missing_ok=True)
                        raise ValueError('Shared download SHA-256 mismatch')
                    partial.replace(target)
                    return target
                except (subprocess.CalledProcessError, ValueError) as error:
                    failure = error
        if failure:
            raise failure


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='Fetch a verified shared installer')
    parser.add_argument('sha256')
    parser.add_argument('suffix')
    parser.add_argument('urls', nargs='+')
    args = parser.parse_args()
    # stdout is reserved for the pathname consumed by shell installers.
    print(download(args.urls, args.sha256, args.suffix))
