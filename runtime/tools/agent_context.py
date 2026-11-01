"""Shared local agent environment and instructions, independent of model provider."""
import os
from pathlib import Path

import desktop_bus


def environment(source):
    env = desktop_bus.environment(source)
    env['PYTHONPATH'] = os.pathsep.join(filter(None, ('/usr/lib/arlinux/python', env.get('PYTHONPATH'))))
    return env


def instructions():
    return (Path(__file__).parent / 'guest/agent-AGENTS.md').read_text()
