"""Shared local agent environment and instructions, independent of model provider."""
import os
from pathlib import Path
import sys

import desktop_bus


def environment(source):
    env = desktop_bus.environment(source)
    env['PYTHONPATH'] = os.pathsep.join(filter(None, ('/usr/lib/arlinux/python', env.get('PYTHONPATH'))))
    return env


def instructions():
    return (Path(__file__).parent / 'guest/agent-AGENTS.md').read_text()


def main():
    if sys.argv[1:] == ['instructions']:
        print(instructions(), end='')
    elif len(sys.argv) > 2 and sys.argv[1] == 'python':
        os.execve('/usr/bin/python3', ['python3', *sys.argv[2:]], environment(os.environ))
    else:
        raise SystemExit('Usage: arlinux-agent instructions | arlinux-agent python SCRIPT.py [ARGS...] | arlinux-agent python -c CODE')


if __name__ == '__main__':
    main()
