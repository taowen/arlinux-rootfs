"""Prepare the pinned WebView2 dependency for Bionic Wine.

The immutable vendor files are shared and copied into the one instance Wine
prefix. Registry settings and GPU flags are applied there through ANGLE Vulkan.
"""
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

import shared
import windows

MANIFEST = Path('/usr/lib/arlinux/guest/windows-app-downloads.tsv')
DOWNLOAD = 'webview2-fixed-arm64'
# Chromium selects its Windows graphics backend and composition capabilities.
# Forcing ANGLE Vulkan prevents its Direct3D shared-image composition path.
WEBVIEW_FLAGS = '--no-sandbox'


def verified_browser(destination, expected):
    record = json.loads((destination/'arlinux-runtime.json').read_text())
    if record.get('protocol') != 1 or record.get('sha256') != expected or not record.get('files'):
        raise RuntimeError('Invalid shared WebView2 runtime record')
    for name, checksum in record['files'].items():
        path = destination/name
        if path.is_symlink() or not path.resolve().is_relative_to(destination.resolve()) or shared.digest(path) != checksum:
            raise RuntimeError('Shared WebView2 runtime integrity check failed')
    browser = destination/record['browser']
    if not browser.resolve().is_relative_to(destination.resolve()) or not (browser/'msedgewebview2.exe').is_file():
        raise RuntimeError('Shared WebView2 executable is missing')
    return browser


def ensure_runtime():
    rows = [line.split('\t') for line in MANIFEST.read_text().splitlines()
            if line.startswith(DOWNLOAD+'\t')]
    if len(rows) != 1 or len(rows[0]) != 3:
        raise ValueError('Missing pinned WebView2 fixed-runtime download')
    _, expected, url = rows[0]
    if not re.fullmatch('[0-9a-f]{64}', expected) or not url.startswith('https://msedge.sf.dl.delivery.mp.microsoft.com/') or not url.endswith('.cab'):
        raise ValueError('Invalid WebView2 fixed-runtime source')
    store = shared.directory('windows-webview2')
    destination = store/expected
    with shared.locked(store/(expected+'.lock')):
        if destination.is_symlink():
            raise RuntimeError('Shared WebView2 runtime must not be redirected')
        if destination.exists():
            return verified_browser(destination, expected)
        archive = shared.download(url, expected, '.cab')
        print('Preparing Microsoft WebView2…', flush=True)
        if not shutil.which('cabextract'):
            subprocess.run(['sudo', 'apt-get', 'update'], check=True, timeout=300)
            subprocess.run(['sudo', 'apt-get', 'install', '-y', 'cabextract'], check=True, timeout=300)
        with tempfile.TemporaryDirectory(prefix='.webview-', dir=store) as temporary:
            stage = Path(temporary)/'runtime'
            stage.mkdir()
            subprocess.run(['cabextract', '-q', '-d', str(stage), str(archive)],
                           check=True, stdout=subprocess.DEVNULL)
            browsers = list(stage.glob('*/msedgewebview2.exe'))
            if len(browsers) != 1:
                raise RuntimeError('Unexpected Microsoft WebView2 archive layout')
            files = {}
            for path in stage.rglob('*'):
                if path.is_symlink():
                    raise RuntimeError('Unexpected link in Microsoft WebView2 runtime')
                if path.is_file():
                    files[path.relative_to(stage).as_posix()] = shared.digest(path)
            record = dict(protocol=1, sha256=expected,
                          browser=browsers[0].parent.relative_to(stage).as_posix(), files=files)
            (stage/'arlinux-runtime.json').write_text(json.dumps(record)+'\n')
            stage.rename(destination)
        return verified_browser(destination, expected)


def install_into_prefix(prefix):
    """Expose the one pinned browser runtime through the instance prefix.

    The immutable download remains outside Wine, while Windows applications
    see one canonical ``C:\\Program Files\\Microsoft\\EdgeWebView`` tree.
    """
    browser = ensure_runtime()
    if not browser.is_dir() or not (browser/'msedgewebview2.exe').is_file():
        raise RuntimeError('Verified WebView2 browser directory is missing')
    destination = (prefix/'pfx/drive_c/Program Files/Microsoft/EdgeWebView/Application'
                   /browser.name)
    # Serialize prefix publication, not the shared immutable download. Never
    # remove another launch's staging directory or replace a partial tree.
    destination.parent.mkdir(parents=True, exist_ok=True)
    with shared.locked(destination.parent/'.install.lock'):
        expected = {p.relative_to(browser): shared.digest(p)
                    for p in browser.rglob('*') if p.is_file()}
        def complete(tree):
            return (not tree.is_symlink() and tree.is_dir() and
                    all(not (tree/name).is_symlink() and (tree/name).is_file()
                        and shared.digest(tree/name) == checksum
                        for name, checksum in expected.items()))
        if destination.exists() or destination.is_symlink():
            if not complete(destination):
                raise RuntimeError('Installed WebView2 runtime is damaged; reinstall the instance')
            return destination
        with tempfile.TemporaryDirectory(prefix='.install-', dir=destination.parent) as temporary:
            stage = Path(temporary)/'browser'
            shutil.copytree(browser, stage)
            if not complete(stage):
                raise RuntimeError('WebView2 copy integrity check failed')
            stage.rename(destination)
    return destination


def browser_windows_path(prefix, browser):
    """Use the canonical C: path, avoiding Android paths exceeding MAX_PATH.

    WebView queries its own image into a MAX_PATH buffer and terminates when
    launched through the much longer Android-backed Z: alias.
    """
    relative = browser.relative_to(prefix / 'pfx/drive_c')
    return 'C:\\' + str(relative).replace('/', '\\')


def prepare(root, prefix, environment):
    """Apply one instance-wide browser policy, independent of game or EXE name."""
    env = dict(environment)
    browser = install_into_prefix(prefix)
    folder = browser_windows_path(prefix, browser)
    # Elevated hosts may ignore environment overrides. Microsoft's '*' policy
    # covers all hosts; set both views so x86 and x64 hosts see the same policy.
    for view in ('/reg:64', '/reg:32'):
        for policy, value in (('AdditionalBrowserArguments', WEBVIEW_FLAGS),
                              ('BrowserExecutableFolder', folder)):
            windows.run(root, prefix, 'reg.exe', ['add',
                'HKLM\\Software\\Policies\\Microsoft\\Edge\\WebView2\\' + policy,
                '/v', '*', '/t', 'REG_SZ', '/d', value, '/f', view],
                check=True, capture=True, timeout=120)
    env['WEBVIEW2_BROWSER_EXECUTABLE_FOLDER'] = folder
    env['WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS'] = WEBVIEW_FLAGS
    return env
