"""Atomic, resumable downloads of immutable build inputs, on Windows and Linux."""
import hashlib
from pathlib import Path
import re
import subprocess
import time


def sha256_file(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def download(url,output,expected,label=None):
    if not re.fullmatch('[0-9a-f]{64}',expected):
        raise ValueError('Build download requires a pinned SHA-256')
    output=Path(output)
    output.parent.mkdir(parents=True,exist_ok=True)
    if output.is_file() and sha256_file(output)==expected:
        return output
    # A complete file from an older fixed release is not a partial download
    # of this generation. Keep resumable bytes bound to the expected digest.
    part=output.with_name(output.name+'.'+expected+'.part')
    if output.is_file():
        output.unlink()
    # A process may stop after receiving the last byte but before renaming.
    if part.is_file() and sha256_file(part)==expected:
        part.replace(output)
        return output
    transfer_url=url
    if url.startswith('https://github.com/') and '/releases/download/' in url:
        transfer_url += ('&' if '?' in url else '?')+'arlinux_sha256='+expected
    error=''
    for attempt in range(8):
        request_url=transfer_url
        if attempt and transfer_url!=url:
            request_url+='&arlinux_retry='+str(time.time_ns())
        command=['curl','--silent','--show-error','--location','--fail',
            '--continue-at','-','--connect-timeout','20','--max-time','600',
            '--speed-time','90','--speed-limit','1024','--output',str(part),request_url]
        started=time.monotonic()
        offset=part.stat().st_size if part.is_file() else 0
        with subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) as process:
            try:
                while True:
                    try:
                        stdout,stderr=process.communicate(timeout=15)
                        break
                    except subprocess.TimeoutExpired:
                        size=part.stat().st_size if part.is_file() else 0
                        rate=max(0,size-offset)/max(1,time.monotonic()-started)/(1024*1024)
                        print(f'Transfer {label or output.name}: {size/(1024*1024):.1f} MiB retained, {rate:.2f} MiB/s',flush=True)
            except BaseException:
                process.kill()
                process.communicate()
                raise
        result=subprocess.CompletedProcess(command,process.returncode,stdout,stderr)
        if result.returncode==0 and part.is_file() and sha256_file(part)==expected:
            part.replace(output)
            return output
        error=result.stderr.strip() or 'SHA-256 mismatch'
        if result.returncode in (0,33) or (result.returncode==22 and '416' in result.stderr):
            # The server rejected ranges, or a complete file was corrupted.
            part.unlink(missing_ok=True)
        if attempt<7:
            print(f'Retry verified download {output.name}: {error[-200:]}',flush=True)
            time.sleep(min(2**attempt,16))
    raise RuntimeError('Cannot obtain verified build input '+str(output)+': '+error)


def verified_product(output, reference):
    """Validate a cached final product against its exact component reference."""
    import json
    output = Path(output)
    try:
        receipt = json.loads((output/'download-receipt.json').read_text())
        if receipt['reference'] != reference or not receipt['files']:
            return None
        for name, expected in receipt['files'].items():
            if ('\\' in name or ':' in name or name.startswith('/') or
                    any(p in ('', '.', '..') for p in name.split('/'))):
                return None
            file = output/name
            if not file.is_file() or sha256_file(file) != expected:
                return None
        return receipt
    except (OSError, ValueError, KeyError):
        return None


def reuse_product(output, reference):
    """Reuse identical verified components across isolated integration outputs."""
    import json
    import shutil
    output = Path(output)
    if verified_product(output, reference):
        print('OK verified local product '+str(output), flush=True)
        return True
    # Only inspect the canonical content-verified build cache, never arbitrary
    # release ZIPs, exported instances or a product selected by its filename.
    if not re.fullmatch('[0-9a-f]{40}', output.parent.name) or output.parent.parent.name != 'products':
        return False
    for receipt_path in output.parent.parent.glob('*/'+output.name+'/download-receipt.json'):
        source = receipt_path.parent
        if source == output:
            continue
        try:
            candidate = json.loads(receipt_path.read_text())
        except (OSError, ValueError):
            continue
        if candidate.get('reference') != reference:
            continue
        receipt = verified_product(source, reference)
        if not receipt:
            continue
        output.mkdir(parents=True, exist_ok=True)
        for name, expected in receipt['files'].items():
            destination = output/name
            destination.parent.mkdir(parents=True, exist_ok=True)
            pending = destination.with_name(destination.name+'.copy-part')
            shutil.copyfile(source/name, pending)
            if sha256_file(pending) != expected:
                raise ValueError('Cached product changed while copying: '+name)
            pending.replace(destination)
        (output/'download-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
        print('OK identical verified component '+reference['identity']+' reused from '+str(source)+'; network download skipped', flush=True)
        return True
    return False


def download_capsule(record, directory):
    """Keep verified transfers and partial files across interrupted invocations."""
    import shutil
    directory = Path(directory)
    expected = record['archiveSha256']
    if not re.fullmatch('[0-9a-f]{64}', expected):
        raise ValueError('Invalid product capsule checksum')
    archive = directory/'capsule.tar.zst'
    if archive.is_file() and sha256_file(archive) == expected:
        print('OK verified cached capsule '+expected+'; network download skipped', flush=True)
        return archive
    from concurrent.futures import ThreadPoolExecutor
    unique = {}
    for part in record['parts']:
        checksum = part['sha256']
        if not re.fullmatch('[0-9a-f]{64}', checksum):
            raise ValueError('Invalid product part checksum')
        unique.setdefault(checksum, part)
    def transfer(item):
        checksum, part = item
        file = directory/'parts'/checksum
        partial = file.with_name(file.name+'.'+checksum+'.part')
        if not file.is_file():
            resumed = partial.stat().st_size if partial.is_file() else 0
            print(f'Download capsule part {part["name"]}; resume offset {resumed} bytes', flush=True)
        return checksum, download(part['url'], file, checksum, label=part['name'])
    with ThreadPoolExecutor(max_workers=min(4, max(1, len(unique)))) as pool:
        completed = dict(pool.map(transfer, unique.items()))
    pieces = [completed[part['sha256']] for part in record['parts']]
    pending = archive.with_name(archive.name+'.assembling')
    with pending.open('wb') as dest:
        for file in pieces:
            with file.open('rb') as source:
                shutil.copyfileobj(source, dest)
    if sha256_file(pending) != expected:
        raise ValueError('Product capsule SHA-256 mismatch')
    pending.replace(archive)
    # The complete verified archive now owns these bytes; do not store both
    # every part and another full concatenated copy after a successful transfer.
    for file in set(pieces):
        file.unlink()
    return archive
