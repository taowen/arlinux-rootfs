"""The desktop Tasks UI's existing task ownership and window operations.

Task IDs identify owned application lifetimes; window IDs identify individual
compositor windows. Neither is a PID or a title. Operations confirm delivery;
inspect the resulting window and accessibility tree before continuing.
"""
from __future__ import annotations

from functools import lru_cache
import importlib.util
from pathlib import Path


@lru_cache(maxsize=1)
def _shell():
    # Share the Tasks UI implementation, including its Classic/Yibu routing.
    path = Path(__file__).resolve().parents[2] / 'guest' / 'shell_tasks.py'
    spec = importlib.util.spec_from_file_location('_arlinux_shell_tasks', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def running() -> list[dict]:
    """Return owned tasks with id, title, kind and app_id; IDs can be stopped."""
    return [dict(id=t['id'], title=t['title'], kind=t['kind'], app_id=t['appId'])
            for t in _shell().task_request('list')['tasks']]


def windows() -> list[dict]:
    """Return mapped windows with id, title, app_id, task_id and focus_order.

    Smaller focus_order means more recently focused. An empty task_id means the
    window has no managed task owner: close it by window ID, never signal its PID.
    A task can own multiple windows, so select explicitly before operating.
    """
    shell = _shell()
    owners = shell.task_request('list')['tasks']
    result = []
    for window in shell.clients():
        owner = next((t for t in owners if window.get('pid') in t['pids']), None)
        if owner is None:
            # Hosted windows belong to the Android host process, while task
            # process ownership describes the guest. Match its declared app ID
            # only when unique; multiple launches must remain unassociated.
            matches = [t for t in owners if t['appId'] and t['appId'] == window.get('class')]
            if len(matches) == 1:
                owner = matches[0]
        result.append(dict(id=shell.identity(window), title=window.get('title', ''),
                           app_id=window.get('class', ''),
                           task_id=owner['id'] if owner else '',
                           focus_order=window.get('focusHistoryID', 9999),
                           fullscreen=bool(window.get('fullscreen') or window.get('fullscreenClient'))))
    return result


def focus(window_id: str) -> None:
    """Focus an ID from windows(); then re-read the target's accessibility tree."""
    _shell().operate(window_id, 'focus')


def maximize(window_id: str) -> None:
    """Toggle maximization using the same operation as the Tasks UI."""
    _shell().operate(window_id, 'maximize')


def close(window_id: str) -> None:
    """Request normal window close; the application may show a save dialog."""
    _shell().operate(window_id, 'close')


def stop(task_id: str) -> None:
    """End an owned task from running(), including its owned process tree.

    This can discard unsaved work. Use only for an application the user asked
    to end, or a task you started for the current request. Do not stop shared
    desktop services or OpenCode itself while carrying out another task.
    """
    _shell().task_request('stop', id=task_id)
