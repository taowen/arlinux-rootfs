"""Stage the checksum-verified optional capture product for developer deployment."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import zipfile

product, destination = map(Path, sys.argv[1:])
archive = product/'capture.zip'
with archive.open('rb') as stream:
    actual = hashlib.file_digest(stream, 'sha256').hexdigest()
if actual != json.loads((product/'capture.json').read_text())['archiveSha256']:
    raise ValueError('Capture product checksum mismatch')
with zipfile.ZipFile(archive) as source:
    for member in source.infolist():
        name = PurePosixPath(member.filename)
        if (name.is_absolute() or '..' in name.parts or '\\' in member.filename
                or not name.parts or name.parts[0] != 'capture-tools'):
            raise ValueError('Unsafe capture product member')
        path = destination.joinpath(*name.parts[1:])
        if member.is_dir():
            path.mkdir(parents=True, exist_ok=True)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(source.read(member))
            path.chmod((member.external_attr >> 16) & 0o777)
