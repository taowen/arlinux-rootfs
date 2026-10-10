#!/usr/bin/env python3
"""Capture first-chance exceptions with a read-only Windows debug-event observer.

Run inside the guest. This uses Proton's existing runinprefix entry point
and Windows debugging APIs without changing thread contexts. Logs may contain private
application data; review them before sharing. Detaching resumes the application.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, required=True, help='Existing Proton data directory (contains pfx)')
    parser.add_argument('--output', type=Path, required=True, help='New private report directory')
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--observer', type=Path, required=True, help='ARM64 exception-observer.exe built from the GE fork')
    parser.add_argument('executable', type=Path)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 600:
        parser.error('Use 1..600 seconds')
    prefix = args.prefix.expanduser().resolve(strict=True)
    if not (prefix/'pfx').is_dir():
        parser.error('--prefix must contain an existing pfx directory')
    executable = args.executable.expanduser().resolve(strict=True)
    if not executable.is_file():
        parser.error('Expected an executable file')
    storage = Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'arlinux/windows'
    current = json.loads((storage/'current.json').read_text())
    runtime = storage/'runtimes'/current['sha256']
    debugger = args.observer.expanduser().resolve(strict=True)
    if not debugger.is_file():
        parser.error('Observer executable not found')
    report = args.output.expanduser()
    report.mkdir(parents=True, mode=0o700, exist_ok=False)
    report = report.resolve()
    env = os.environ.copy()
    for name in ('LD_PRELOAD', 'FEX_ROOTFS', 'FEX_ENV', 'FEX_APP_CONFIG',
                 'FEX_APP_CONFIG_LOCATION', 'WINEPREFIX', 'WINELOADER',
                 'WINESERVER', 'WINEDLLPATH', 'WINEARCH'):
        env.pop(name, None)
    env.update(STEAM_COMPAT_DATA_PATH=str(prefix),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(storage/'client'),
               SteamAppId='0', SteamGameId='0', PROTON_USE_XALIA='0')
    env.setdefault('WINEDEBUG', '-all')
    command = [str(runtime/'proton'), 'runinprefix', str(debugger),
               str(args.seconds), 'Z:'+str(executable).replace('/', '\\')]
    metadata = {'runtime_sha256': current['sha256'], 'prefix': str(prefix),
                'executable': str(executable), 'started': time.time(),
                'observer_sha256': hashlib.sha256(debugger.read_bytes()).hexdigest(),
                'seconds': args.seconds}
    with executable.open('rb') as binary:
        metadata['executable_sha256'] = hashlib.file_digest(binary, 'sha256').hexdigest()
    (report/'runtime.json').write_text((runtime/'arlinux-runtime.json').read_text())
    timed_out = False
    with (report/'debugger.log').open('w') as log:
        process = subprocess.Popen(command, cwd=executable.parent, env=env,
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            status = process.wait(timeout=args.seconds+30)
        except subprocess.TimeoutExpired:
            timed_out = True
            # Only this diagnostic launch, never every process in the prefix.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                status = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                status = process.wait()
    metadata.update(exit_code=status, timed_out=timed_out, finished=time.time())
    (report/'report.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(json.dumps({'report': str(report), 'exit_code': status, 'timed_out': timed_out}))
    return 124 if timed_out else status


if __name__ == '__main__':
    raise SystemExit(main())
