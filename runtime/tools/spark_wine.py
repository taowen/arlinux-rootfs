"""Import portable Spark Windows applications into the instance Bionic prefix.

Vendor Wine bottles, registry snapshots and Linux .so patches are not imported.
Only a statically declared application under Program Files is supported.
"""
import argparse
import configparser
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shlex
import shutil
import subprocess
import tempfile
import shared
import windows


def store():
    path = Path.home()/'.local/share/arlinux/spark-wine'
    path.mkdir(parents=True, exist_ok=True)
    return path


def checked_name(name):
    if not re.fullmatch(r'[a-z0-9][a-z0-9+.-]{0,127}', name):
        raise ValueError('Invalid Spark package name')
    return name


def desktop_path(name):
    return Path.home()/'.local/share/applications'/('arlinux-spark-'+checked_name(name)+'.desktop')


def application_path(relative):
    path = PurePosixPath(relative)
    if len(path.parts) != 2 or path.parts[0].lower() not in ('program files', 'program files (x86)') or '..' in path.parts:
        raise ValueError('Invalid imported application directory')
    drive = windows.instance_prefix()/'pfx/drive_c'
    target = drive/path
    if not target.resolve().is_relative_to(drive.resolve()):
        raise ValueError('Imported application escapes the Wine drive')
    return target


def record(name):
    path = store()/(checked_name(name)+'.json')
    if not path.is_file():
        return None
    value = json.loads(path.read_text())
    root = application_path(value['directory'])
    if not (root/value['executable']).resolve().is_relative_to(root.resolve()):
        raise ValueError('Imported executable escapes its application')
    return value


def installed():
    result = {}
    for path in sorted(store().glob('*.json')):
        value = record(path.stem)
        if (application_path(value['directory'])/value['executable']).is_file():
            result[path.stem] = value['version']
    return result


def assignment(script, name, required=True):
    values = []
    for line in script.splitlines():
        match = re.fullmatch(r'\s*(?:export\s+)?'+name+r'=(.*)', line)
        if match is None:
            continue
        if '$' in match[1] or '`' in match[1]:
            raise ValueError('Dynamic Spark launcher settings are unsupported: '+name)
        parts = shlex.split(match[1], comments=True)
        if len(parts) > 1:
            raise ValueError('Invalid Spark launcher setting: '+name)
        values.append(parts[0] if parts else '')
    if len(set(values)) > 1 or (required and (not values or not values[0])):
        raise ValueError('Expected one static Spark launcher setting: '+name)
    return values[0] if values else ''


def archive_entries(archive):
    output = subprocess.check_output(['7z', 'l', '-slt', '-ba', str(archive)], text=True)
    entries = []
    for paragraph in output.split('\n\n'):
        entry = dict(line.split(' = ', 1) for line in paragraph.splitlines() if ' = ' in line)
        if 'Path' not in entry:
            continue
        path = PurePosixPath(entry['Path'].replace('\\', '/'))
        entries.append((path, entry))
    return entries


def quote(argument):
    return '"'+str(argument).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$')+'"'


def install(deb):
    fields = subprocess.check_output(['dpkg-deb', '-f', str(deb), 'Package', 'Version'], text=True).splitlines()
    name = checked_name(fields[0].removeprefix('Package: ').strip())
    version = fields[1].removeprefix('Version: ').strip()
    if not shutil.which('7z'):
        subprocess.run(['sudo', 'apt-get', 'update'], check=True)
        subprocess.run(['sudo', 'apt-get', 'install', '-y', '--no-install-recommends', '7zip'], check=True)
    with shared.locked(store()/'install.lock'), tempfile.TemporaryDirectory(prefix='spark-wine-') as temporary:
        stage = Path(temporary)
        subprocess.run(['dpkg-deb', '--extract', str(deb), str(stage/'deb')], check=True)
        files = stage/'deb/opt/apps'/name/'files'
        script = (files/'run.sh').read_text()
        executable = PureWindowsPath(assignment(script, 'EXEC_PATH'))
        if executable.drive.lower() != 'c:' or not executable.is_absolute() or '..' in executable.parts:
            raise ValueError('Only a declared executable on the C: drive is supported')
        relative = PurePosixPath(*executable.parts[1:])
        if len(relative.parts) < 3 or relative.parts[0].lower() not in ('program files', 'program files (x86)'):
            raise ValueError('Spark application must be portable under Program Files')
        if assignment(script, 'INSTALL_SETUP', required=False):
            raise ValueError('Spark packages requiring a vendor installer are unsupported')
        overrides = assignment(script, 'WINEDLLOVERRIDES', required=False)
        if not re.fullmatch(r'[A-Za-z0-9_,;= .-]*', overrides):
            raise ValueError('Unsupported Spark DLL overrides')
        archive = files/'files.7z'
        suffix = ('drive_c/'+relative.as_posix()).lower()
        members = archive_entries(archive)
        matches = [path for path, _ in members if path.as_posix().lower().endswith(suffix)]
        if len(matches) != 1:
            raise ValueError('Declared Spark executable is missing or ambiguous')
        source_exe = matches[0]
        source_root = PurePosixPath(*source_exe.parts[:len(source_exe.parts)-len(relative.parts)+2])
        if source_root.is_absolute() or '..' in source_root.parts or ':' in str(source_root):
            raise ValueError('Unsafe Spark application path')
        if any(character in str(source_root) for character in '*?[]'):
            raise ValueError('Unsupported application directory name')
        # Other bottle trees include Wine drive symlinks. They are deliberately
        # excluded, not extracted and then repaired or merged into our prefix.
        for path, entry in members:
            if not path.is_relative_to(source_root):
                continue
            if '..' in path.parts or ':' in str(path):
                raise ValueError('Unsafe Spark application member')
            if 'Symbolic Link' in entry or 'Hard Link' in entry or 'L' in entry.get('Attributes', '').upper():
                raise ValueError('Linked application files are unsupported')
        subprocess.run(['7z', 'x', '-y', '-o'+str(stage/'payload'), str(archive), str(source_root)+'/*'], check=True)
        source = stage/'payload'/source_root
        if source.is_symlink() or any(path.is_symlink() for path in source.rglob('*')):
            raise ValueError('Linked application files are unsupported')
        entries = sorted((stage/'deb/opt/apps'/name/'entries/applications').glob('*.desktop'))
        if len(entries) != 1:
            raise ValueError('Expected one application desktop entry')
        entry = configparser.ConfigParser(interpolation=None)
        entry.read(entries[0])
        title = entry['Desktop Entry']['Name']
        root = windows.ensure_runtime()
        windows.initialize(root, windows.instance_prefix())
        directory = '/'.join(relative.parts[:2])
        destination = application_path(directory)
        previous = record(name)
        if previous is not None and previous['directory'] != directory:
            raise ValueError('Application directory changed; remove the previous import first')
        if destination.exists() and previous is None:
            raise ValueError('Application directory is already owned by another Windows installation')
        destination.parent.mkdir(parents=True, exist_ok=True)
        desktop = desktop_path(name)
        previous_desktop = desktop.read_bytes() if desktop.is_file() else None
        with tempfile.TemporaryDirectory(prefix='.spark-', dir=destination.parent) as transaction:
            transaction = Path(transaction)
            shutil.copytree(source, transaction/'new')
            backup = transaction/'old'
            if destination.exists():
                destination.rename(backup)
            try:
                (transaction/'new').rename(destination)
                exe = destination/source_exe.relative_to(source_root)
                if not exe.is_file():
                    raise ValueError('Extracted Spark executable is missing')
                command = ['arlinux-windows']
                if overrides:
                    command += ['--dll-overrides', overrides]
                command.append(str(exe))
                desktop.parent.mkdir(parents=True, exist_ok=True)
                desktop.write_text('[Desktop Entry]\nType=Application\nName='+title+'\nExec='+
                    ' '.join(map(quote, command))+'\nCategories=Utility;\nStartupWMClass='+exe.name+'\n')
                receipt = store()/(name+'.json')
                pending = receipt.with_suffix('.tmp')
                pending.write_text(json.dumps(dict(version=version, directory=directory,
                    executable=str(exe.relative_to(destination)), sha256=shared.digest(deb)))+'\n')
                pending.replace(receipt)
            except BaseException:
                if destination.exists():
                    shutil.rmtree(destination)
                if backup.exists():
                    backup.rename(destination)
                if previous_desktop is None:
                    desktop.unlink(missing_ok=True)
                else:
                    desktop.write_bytes(previous_desktop)
                raise
        print('Imported '+title+' using the instance Bionic Wine prefix', flush=True)


def remove(name):
    with shared.locked(store()/'install.lock'):
        value = record(name)
        if value is None:
            raise ValueError('Spark Windows application is not installed')
        shutil.rmtree(application_path(value['directory']))
        desktop_path(name).unlink(missing_ok=True)
        (store()/(name+'.json')).unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('list', 'install', 'remove', 'entries'))
    parser.add_argument('value', nargs='?')
    args = parser.parse_args()
    if args.operation == 'list':
        print(json.dumps(installed()))
    elif args.operation == 'install':
        install(Path(args.value).resolve(strict=True))
    elif args.operation == 'remove':
        remove(checked_name(args.value))
    elif record(checked_name(args.value)) is not None:
        print(desktop_path(args.value))


if __name__ == '__main__':
    main()
