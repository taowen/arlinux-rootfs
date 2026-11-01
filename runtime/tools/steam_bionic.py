"""Steam compatibility-tool entry point for Android-hosted Bionic Wine.

Steam launches a guest Python adapter; only the Android host executes Wine.
Runtime installation uses the pinned Windows manifest and immutable shared cache.
"""
import argparse
import json
import os
import re
import shutil
from pathlib import Path
import sys
import tempfile

TOOL = 'arlinux-wine-bionic'
ENVIRONMENT = ('WINEDEBUG', 'ARLINUX_EXCEPTION_LOG', 'ARLINUX_VULKAN_CAPTURE', 'SteamAppId', 'SteamGameId', 'SteamOverlayGameId',
               'WEBVIEW2_BROWSER_EXECUTABLE_FOLDER',
               'WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS')


def register(steam):
    tool = steam/'compatibilitytools.d'/TOOL
    expected = {'backend': 'bionic', 'protocol': 1}
    if tool.exists():
        marker = tool/'arlinux-backend.json'
        if not marker.is_file() or json.loads(marker.read_text()) != expected:
            raise RuntimeError('Preserving unrecognized compatibility tool: '+str(tool))
        return tool
    tool.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.wine-bionic-', dir=tool.parent) as temporary:
        stage = Path(temporary)/'tool'
        stage.mkdir()
        (stage/'arlinux-backend.json').write_text(json.dumps(expected)+'\n')
        (stage/'run').write_text('#!/bin/sh\nexec python3 /usr/lib/arlinux/steam_bionic.py launch "$@"\n')
        (stage/'run').chmod(0o755)
        (stage/'toolmanifest.vdf').write_text('"manifest"\n{\n "version" "2"\n "commandline" "/run %verb%"\n "use_sessions" "0"\n}\n')
        (stage/'compatibilitytool.vdf').write_text('"compatibilitytools"\n{\n "compat_tools"\n {\n  "'+TOOL+'"\n  {\n   "install_path" "."\n   "display_name" "ARLinux Wine (Bionic, experimental)"\n   "from_oslist" "windows"\n   "to_oslist" "linux"\n  }\n }\n}\n')
        stage.rename(tool)
    return tool


def configure(steam):
    """Register Android-hosted Bionic Wine before Steam scans custom tools."""
    register(steam)
    select_default(steam)


def select_default(steam):
    """Select Bionic before client startup, preserving unrelated user choices."""
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            executable = (process/'cmdline').read_bytes().split(b'\0', 1)[0]
            if executable == os.fsencode(steam/'steamrtarm64/steam'):
                return
        except OSError:
            continue
    config = steam/'config/config.vdf'
    existed = config.is_file()
    config.parent.mkdir(parents=True, exist_ok=True)
    original = config.read_text() if existed else ('"InstallConfigStore" { '
        '"Software" { "Valve" { "Steam" { } } } }\n')
    updated = default_config(original)
    if updated == original:
        return
    backup = config.with_name('config.vdf.before-arlinux-bionic')
    if existed and not backup.exists():
        shutil.copy2(config, backup)
    with tempfile.NamedTemporaryFile(mode='w', dir=config.parent, delete=False) as out:
        temporary = Path(out.name)
        out.write(updated)
    try:
        temporary.chmod(config.stat().st_mode & 0o777 if existed else 0o600)
        if (existed and config.read_text() != original) or (not existed and config.exists()):
            raise RuntimeError('Steam configuration changed during setup; retry with Steam closed')
        temporary.replace(config)
    finally:
        temporary.unlink(missing_ok=True)


def default_config(text):
    """Edit only CompatToolMapping; leave all other bytes and fields untouched."""
    from steam import keyvalues
    keyvalues(text)
    tokens = list(re.finditer(r'"(?:\\.|[^"\\])*"|[{}]', text))
    stack, spans = [], {}
    for i, token in enumerate(tokens):
        if token.group() == '{':
            stack.append((json.loads(tokens[i-1].group()), token.end()))
        elif token.group() == '}':
            path = tuple(name.lower() for name, _ in stack)
            spans[path] = (stack[-1][1], token.start())
            stack.pop()
    parent = ('installconfigstore', 'software', 'valve', 'steam')
    span = spans.get((*parent, 'compattoolmapping'))
    if span:
        start, end = span
        mapping = keyvalues(text[start:end])
    elif parent in spans:
        start = end = spans[parent][1]
        mapping = {}
    else:
        raise RuntimeError('Steam configuration has no InstallConfigStore/Software/Valve/Steam')
    changed = False
    for app, entry in list(mapping.items()) + ([] if '0' in mapping else [('0', {})]):
        name = entry.get('name', '')
        # Prefer Windows depots even when Steam offers a native Linux build.
        # Priority 75 is only a fallback and loses to native compatibility rules.
        # Keep explicit per-game choices, including native Linux selections.
        if name == 'arlinux-proton-arm64' or (app == '0' and
                (not name or name == TOOL or name.lower() == 'native' or name.lower().startswith('proton_'))):
            selected = dict(entry, name=TOOL)
            selected.setdefault('config', '')
            if app == '0':
                selected['priority'] = '250'
            else:
                selected.setdefault('priority', '250')
            if selected != entry:
                mapping[app] = selected
                changed = True
    if not changed:
        return text
    def encode(data, indent):
        lines = []
        for key, value in data.items():
            if isinstance(value, dict):
                lines.extend([indent+json.dumps(key), indent+'{', encode(value, indent+'\t'), indent+'}'])
            else:
                lines.append(indent+json.dumps(key)+'\t\t'+json.dumps(value))
        return '\n'.join(lines)
    replacement = '\n'+encode(mapping, '\t'*5)+'\n'+'\t'*4
    if not span:
        replacement = '\n\t\t\t\t"CompatToolMapping"\n\t\t\t\t{'+replacement+'}\n'
    return text[:start]+replacement+text[end:]


def launch(verb, arguments):
    import windows
    if verb not in ('run', 'waitforexitandrun', 'getcompatpath', 'getnativepath'):
        raise ValueError('Unsupported Steam compatibility operation: '+verb)
    if not arguments:
        raise ValueError('Steam did not provide an executable or path')
    if verb == 'getcompatpath':
        print('Z:'+str(windows.host_path(arguments[0])).replace('/', '\\'))
        return 0
    if verb == 'getnativepath':
        value = arguments[0]
        if value[:3].lower() != 'z:\\':
            raise ValueError('Only the Bionic host Z: drive can be translated')
        print(windows.guest_path(value[2:].replace('\\', '/')))
        return 0
    data = os.environ.get('STEAM_COMPAT_DATA_PATH', '')
    if not data or not Path(data).is_absolute():
        raise ValueError('Steam must provide an absolute STEAM_COMPAT_DATA_PATH')
    # Steam's compat-data directory contains the game files and metadata.  It
    # is not a Wine prefix: every Bionic-managed Windows app in this Linux
    # instance uses the same instance-level prefix and Program Files tree.
    wine_prefix = windows.instance_prefix()
    env = {name: os.environ[name] for name in ENVIRONMENT if name in os.environ}
    for name, value in env.items():
        if name.startswith('Steam') and not value.isdecimal():
            raise ValueError('Invalid Steam game identity: '+name)
    root = windows.ensure_runtime()
    # The Android host owns initialization, DXVK setup and native driver paths.
    # Steam also runs discovery helpers without an authenticated game session.
    if not env.get('SteamAppId', '0').strip('0'):
        return windows.run(root, wine_prefix, arguments[0], arguments[1:],
                           env=env, timeout=7*24*60*60).returncode
    from windows_webview import prepare
    env = prepare(root, wine_prefix, env)
    import steamworks
    with steamworks.session(root, wine_prefix, env) as launcher:
        result = windows.run(root, wine_prefix, launcher, arguments, env=env,
                             timeout=7*24*60*60, task=True)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    install = commands.add_parser('register')
    install.add_argument('steam', type=Path)
    run = commands.add_parser('launch')
    run.add_argument('verb')
    run.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command == 'register':
        print(register(args.steam))
        return 0
    return launch(args.verb, args.arguments)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print('ARLinux Bionic compatibility: '+str(error), file=sys.stderr)
        raise SystemExit(1)
