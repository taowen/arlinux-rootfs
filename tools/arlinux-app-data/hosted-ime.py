#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""IBus engine for a hosted input method; composition UI belongs to the host.

Run inside the desktop session. Start the standard IBus daemon if needed. The private
Unix stream socket carries newline-delimited JSON. The server publishes a focus
token; edits must echo it. A token expires on every focus transition, including
returning to the same input context. Text and key events share one IBus connection.
"""

import json
import fcntl
import os
import secrets
import socket
import stat
import struct
import signal
import subprocess

import gi
gi.require_version('IBus', '1.0')
from gi.repository import Gio, GLib, IBus

LIMIT = 65536


class Engine(IBus.Engine):
    __gtype_name__ = 'ArlinuxHostedInputEngine'

    def __init__(self, bridge, **kwargs):
        super().__init__(has_focus_id=True, **kwargs)
        self.bridge = bridge
        self.preedit = ''

    def do_focus_in_id(self, context, client):
        self.bridge.focus(self if client != 'fake' else None)

    def do_focus_out_id(self, context):
        if self.bridge.active is self:
            self.bridge.focus(None)
        self.preedit = ''

    def do_process_key_event(self, keyval, keycode, state):
        return False

    def do_reset(self):
        self.set_preedit('')
        if self.bridge.active is self:
            self.bridge.focus(self)

    def do_disable(self):
        self.set_preedit('')
        if self.bridge.active is self:
            self.bridge.focus(None)

    def edit(self, message):
        operation = message['operation']
        text = message.get('text', '')
        if not isinstance(text, str):
            raise ValueError('text must be a string')
        if operation == 'commit':
            self.set_preedit('')
            self.commit_text(IBus.Text.new_from_string(text))
        elif operation == 'preedit':
            self.set_preedit(text)
        elif operation == 'finish':
            text = self.preedit
            self.set_preedit('')
            if text:
                self.commit_text(IBus.Text.new_from_string(text))
        elif operation == 'key':
            values = [message[name] for name in ('keyval', 'keycode', 'state')]
            if any(type(value) is not int or not 0 <= value <= 0xffffffff for value in values):
                raise ValueError('invalid key event')
            self.forward_key_event(*values)
        elif operation == 'delete':
            counts = [message[name] for name in ('before', 'after')]
            if any(type(count) is not int or not 0 <= count <= 1024 for count in counts):
                raise ValueError('invalid deletion length')
            for count, keyval, keycode in ((counts[0], IBus.KEY_BackSpace, 14),
                                          (counts[1], IBus.KEY_Delete, 111)):
                for _ in range(count):
                    self.tap(keyval, keycode)
        elif operation == 'enter':
            self.edit({'operation': 'finish'})
            self.tap(IBus.KEY_Return, 28)
        else:
            raise ValueError('unsupported operation')

    def tap(self, keyval, keycode):
        self.forward_key_event(keyval, keycode, 0)
        self.forward_key_event(keyval, keycode, int(IBus.ModifierType.RELEASE_MASK))

    def set_preedit(self, text):
        self.preedit = text
        self.update_preedit_text(IBus.Text.new_from_string(text), len(text), bool(text))


class Client:
    def __init__(self, bridge, channel):
        self.bridge, self.channel = bridge, channel
        self.pending = bytearray()
        self.watch = GLib.io_add_watch(channel.fileno(), GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self.read)

    def send(self, message):
        try:
            self.channel.sendall(json.dumps(message, ensure_ascii=False).encode() + b'\n')
        except (OSError, UnicodeError):
            # A host that cannot consume small control replies must reconnect;
            # never block the desktop's input loop or replay uncertain edits.
            self.close()

    def read(self, fd, condition):
        try:
            data = self.channel.recv(LIMIT)
            if not data:
                self.close()
                return False
            self.pending.extend(data)
            if len(self.pending) > LIMIT:
                raise ValueError('edit exceeds size limit')
            while b'\n' in self.pending:
                line, _, remainder = self.pending.partition(b'\n')
                self.pending = bytearray(remainder)
                message = json.loads(line)
                if not isinstance(message, dict):
                    raise ValueError('expected an edit object')
                if not self.bridge.active or message.get('focus') != self.bridge.token:
                    self.send({'accepted': False, 'reason': 'focus-changed'})
                    continue
                self.bridge.active.edit(message)
                self.send({'accepted': True})
            return self.channel.fileno() >= 0
        except (OSError, ValueError, KeyError, TypeError):
            self.close()
            return False

    def close(self):
        if self.channel.fileno() < 0:
            return
        GLib.source_remove(self.watch)
        self.channel.close()
        self.bridge.clients.discard(self)


class Bridge:
    def __init__(self, bus):
        self.bus = bus
        directory = os.environ['XDG_RUNTIME_DIR']
        metadata = os.stat(directory)
        if metadata.st_uid != os.getuid() or stat.S_IMODE(metadata.st_mode) & 0o077:
            raise RuntimeError('XDG_RUNTIME_DIR must be private to the current user')
        self.path = os.path.join(directory, 'hosted-ime.sock')
        # A killed process releases flock even if its socket pathname remains.
        # Keep this inode: unlinking a lock file lets concurrent owners diverge.
        self.lock = os.open(self.path + '.lock',
            os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
        fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        pending = self.path + '.starting'
        for path in (self.path, pending):
            if not os.path.lexists(path):
                continue
            metadata = os.lstat(path)
            if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.getuid():
                raise RuntimeError('Unexpected input endpoint owner or type')
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                probe.settimeout(1)
                try:
                    probe.connect(path)
                except ConnectionRefusedError:
                    os.unlink(path)
                else:
                    raise RuntimeError('Hosted input endpoint is already listening')
        self.listener.bind(pending)
        os.chmod(pending, 0o600)
        self.listener.listen(4)
        self.listener.setblocking(False)
        os.rename(pending, self.path)
        self.clients = set()
        self.engines = []
        self.active = None
        self.token = None
        self.loop = GLib.MainLoop()
        GLib.io_add_watch(self.listener.fileno(), GLib.IO_IN, self.accept)
        self.factory = IBus.Factory.new(self.bus.get_connection())
        self.factory.connect('create-engine', self.create_engine)
        self.component = IBus.Component.new('org.freedesktop.IBus.ArlinuxHosted',
            'Hosted input method', '1', 'GPL-3.0-or-later', 'Arlinux', '', '', '')
        self.component.add_engine(IBus.EngineDesc.new('arlinux-hosted', 'Hosted input',
            'Text from the host input method', 'en', 'GPL-3.0-or-later', 'Arlinux', '', 'us'))
        if not self.bus.register_component(self.component):
            raise RuntimeError('Cannot register hosted input engine')
        self.bus.connect('disconnected', lambda bus: self.loop.quit())

    def create_engine(self, factory, name):
        if name != 'arlinux-hosted':
            return None
        engine = Engine(self, connection=self.bus.get_connection(), engine_name=name,
            object_path='/org/freedesktop/IBus/Engine/Hosted' + str(len(self.engines)))
        self.engines.append(engine)
        return engine

    def focus(self, engine):
        self.active = engine
        self.token = secrets.token_hex(16) if engine else None
        for client in list(self.clients):
            client.send({'focus': self.token})

    def accept(self, fd, condition):
        channel, _ = self.listener.accept()
        _, uid, _ = struct.unpack('3i', channel.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if uid != os.getuid():
            channel.close()
            return True
        channel.setblocking(False)
        client = Client(self, channel)
        self.clients.add(client)
        client.send({'focus': self.token})
        return True

    def run(self):
        failure = []
        session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, lambda: (self.loop.quit(), False)[1])
        def selected(bus, result, data):
            try:
                if not bus.set_global_engine_async_finish(result):
                    raise RuntimeError('Cannot select hosted input engine')
                # Publish readiness only after IBus and the engine are usable.
                # Standard D-Bus activation then orders desktop startup without
                # sleeps or application-specific launch wrappers.
                reply = session.call_sync('org.freedesktop.DBus',
                    '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName',
                    GLib.Variant('(su)', ('org.arlinux.HostedInput', 4)),
                    None, Gio.DBusCallFlags.NONE, 15000, None)
                if reply.unpack()[0] != 1:
                    raise RuntimeError('Hosted input service already has an owner')
            except Exception as error:
                failure.append(error)
                self.loop.quit()
        self.bus.set_global_engine_async('arlinux-hosted', 15000, None, selected, None)
        try:
            self.loop.run()
        finally:
            for client in list(self.clients):
                client.close()
            self.listener.close()
            os.unlink(self.path)
            os.close(self.lock)
        if failure:
            raise failure[0]


def session_bus():
    IBus.init()
    bus = IBus.Bus.new_async()
    ready = GLib.MainLoop()
    connected = bus.connect('connected', lambda bus: ready.quit())
    timeout = GLib.timeout_add_seconds(15, lambda: (ready.quit(), False)[1])
    try:
        # IBus normally tracks its parent's lifetime. A desktop autostart entry
        # is not that lifetime owner; use its upstream daemon mode, not retries.
        if not bus.is_connected() and not IBus.get_address():
            subprocess.run(['ibus-daemon', '--daemonize', '--single', '--xim',
                            '--emoji-extension=disable'], check=True, timeout=15)
        if not bus.is_connected():
            ready.run()
        if not bus.is_connected():
            raise RuntimeError('IBus did not become ready')
        return bus
    finally:
        bus.disconnect(connected)
        if GLib.MainContext.default().find_source_by_id(timeout):
            GLib.source_remove(timeout)


if __name__ == '__main__':
    Bridge(session_bus()).run()
