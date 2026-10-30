"""Small, optional helpers for navigating large AT-SPI trees.

Results are ordinary ``pyatspi.Accessible`` objects. Use their upstream
interfaces (Action, Text, EditableText, Component) for actual operations.
"""

from __future__ import annotations

import os

import dbus


def _roots(app, root):
    if root is not None:
        if app is not None:
            raise ValueError("pass either app or root, not both")
        return [root]
    import pyatspi

    applications = list(pyatspi.Registry.getDesktop(0))
    if app is None:
        return applications
    needle = app.casefold()
    exact = [node for node in applications if (node.name or "").casefold() == needle]
    matches = exact or [node for node in applications
                        if needle in (node.name or "").casefold()]
    if not matches:
        raise LookupError(f"AT-SPI application not found: {app}")
    return matches


def _walk(roots, max_nodes):
    if max_nodes is not None and max_nodes < 1:
        raise ValueError("max_nodes must be positive when set")
    stack = [(node, 0) for node in reversed(roots)]
    visited = 0
    while stack:
        if max_nodes is not None and visited >= max_nodes:
            raise RuntimeError(f"AT-SPI scan reached {max_nodes} nodes; narrow app or root")
        node, depth = stack.pop()
        visited += 1
        yield node, depth
        try:
            children = node.childCount
            for index in range(children - 1, -1, -1):
                try:
                    child = node.getChildAtIndex(index)
                    if child is not None:
                        stack.append((child, depth + 1))
                except Exception:
                    continue  # A dynamic window can remove a child mid-scan.
        except Exception:
            continue


def _showing(node):
    import pyatspi

    try:
        return node.getState().contains(pyatspi.STATE_SHOWING)
    except Exception:
        return False


def _cached(roots):
    """Read the standard AT-SPI Cache in one D-Bus call per application."""
    address = os.environ.get("AT_SPI_BUS_ADDRESS")
    if not address:
        raise RuntimeError("AT_SPI_BUS_ADDRESS is not set")
    bus = dbus.bus.BusConnection(address)
    for root in roots:
        name = root.app.bus_name
        proxy = bus.get_object(name, "/org/a11y/atspi/cache", introspect=False)
        items = dbus.Interface(proxy, "org.a11y.atspi.Cache").GetItems(timeout=30)
        by_path = {str(item[0][1]): item for item in items}
        root_path = root.path
        depths = {root_path: 0}
        def depth(path):
            if path in depths:
                return depths[path]
            chain = []
            while path not in depths:
                item = by_path.get(path)
                if item is None or path in chain:
                    return None
                chain.append(path)
                path = str(item[2][1])
            result = depths[path]
            for descendant in reversed(chain):
                result += 1
                depths[descendant] = result
            return depths[chain[0]] if chain else result
        for item in items:
            item_depth = depth(str(item[0][1]))
            if item_depth is not None:
                yield root, item, item_depth, by_path


def _resolve(root, item, by_path):
    """Resolve only a matched item to a live upstream pyatspi Accessible."""
    path = str(item[0][1])
    indices = []
    while path != root.path:
        current = by_path[path]
        indices.append(int(current[3]))
        path = str(current[2][1])
    node = root
    for index in reversed(indices):
        node = node.getChildAtIndex(index)
    return node


def _cached_showing(item):
    import pyatspi

    index = int(pyatspi.STATE_SHOWING)
    return bool(int(item[9][index // 32]) & (1 << (index % 32)))


def actions(node) -> list[str]:
    """Return currently advertised action names; action indexes are not stable."""
    try:
        interface = node.queryAction()
    except NotImplementedError:
        return []
    return [interface.getName(i) for i in range(interface.nActions)]


def perform(node, action: str = 'click') -> bool:
    """Deliver one named semantic action, resolving its index at call time.

    Do not cache action indexes: scrolling can insert or remove backward/forward
    actions. This never retries, clicks coordinates or walks to a guessed parent.
    Re-find the target after navigation and verify the resulting UI separately.
    """
    import pyatspi

    state = node.getState()
    if not state.contains(pyatspi.STATE_ENABLED) or not state.contains(pyatspi.STATE_SHOWING):
        return False
    interface = node.queryAction()
    for index in range(interface.nActions):
        if interface.getName(index).casefold() == action.casefold():
            return bool(interface.doAction(index))
    raise LookupError(f'Node does not advertise action {action!r}')


def find(query: str = '', *, app: str | None = None, root=None,
         limit: int = 10, max_nodes: int | None = None, showing: bool = True,
         action: str | None = None):
    """Find accessible nodes by name/description, without printing a whole tree.

    ``app`` selects an application by exact name, then substring. Pass a node
    as ``root`` to search only that subtree. Results are live upstream nodes;
    re-find after a navigation because an app may replace them at any time.
    ``action`` filters by a currently advertised action name. With that filter,
    query may be empty to discover unnamed controls, such as scroll containers.
    """
    needle = query.strip().casefold()
    if (not needle and not action) or limit < 1:
        raise ValueError("pass a query or action, and a positive limit")
    result = []
    roots = _roots(app, root)
    try:
        cached = _cached(roots)
        for visited, (top, item, _, by_path) in enumerate(cached, 1):
            if max_nodes is not None and visited > max_nodes:
                raise RuntimeError(f"AT-SPI scan reached {max_nodes} nodes; narrow app or root")
            if showing and not _cached_showing(item):
                continue
            if needle not in str(item[6]).casefold() and needle not in str(item[8]).casefold():
                continue
            try:
                node = _resolve(top, item, by_path)
                if action is not None and action.casefold() not in [name.casefold() for name in actions(node)]:
                    continue
                result.append(node)
            except Exception:
                continue  # The UI changed after its cache was read.
            if len(result) >= limit:
                return result
        return result
    except (ImportError, AttributeError, dbus.DBusException):
        pass  # A legacy application may not implement the standard Cache.
    for node, _ in _walk(roots, max_nodes):
        try:
            label = node.name or ""
            matches = needle in label.casefold() or needle in (node.description or "").casefold()
            if matches and (not showing or _showing(node)):
                if action is not None and action.casefold() not in [name.casefold() for name in actions(node)]:
                    continue
                result.append(node)
                if len(result) >= limit:
                    break
        except Exception:
            continue
    return result


def describe(*, app: str | None = None, root=None,
             max_lines: int = 60, max_chars: int = 4200,
             max_nodes: int | None = None, showing: bool = True) -> str:
    """Return a bounded, readable overview of semantic controls and labels.

    Unnamed structural containers are omitted, but their descendants remain
    searchable. For more detail, call ``describe(root=some_node)`` or ``find``.
    The limits bound model output, not the Android accessibility source tree.
    """
    if max_lines < 1 or max_chars < 1:
        raise ValueError("max_lines and max_chars must be positive")
    lines = []
    chars = 0
    truncated = False
    roots = _roots(app, root)
    try:
        from gi.repository import Atspi

        for visited, (_, item, depth, _) in enumerate(_cached(roots), 1):
            if max_nodes is not None and visited > max_nodes:
                raise RuntimeError(f"AT-SPI scan reached {max_nodes} nodes; narrow app or root")
            if showing and depth > 0 and not _cached_showing(item):
                continue
            role = Atspi.role_get_name(Atspi.Role(int(item[7]))) or "node"
            name = str(item[6]).replace("\n", " ").strip()
            if len(name) > 80 and name.startswith(("http://", "https://", "img?")):
                name = "[opaque value omitted]"
            if not name and role.lower() not in ("entry", "document", "document web"):
                continue
            line = f"{role}: {name[:120]}".rstrip()
            if len(lines) >= max_lines or chars + len(line) + 1 > max_chars:
                truncated = True
                break
            lines.append(line)
            chars += len(line) + 1
        if truncated:
            lines.append("… more nodes; use find(query, app=...) or describe(root=node)")
        return "\n".join(lines)
    except (ImportError, AttributeError, dbus.DBusException):
        pass  # A legacy application may not implement the standard Cache.
    for node, depth in _walk(roots, max_nodes):
        try:
            role = node.getRoleName() or "node"
            name = (node.name or "").replace("\n", " ").strip()
            if len(name) > 80 and name.startswith(("http://", "https://", "img?")):
                name = "[opaque value omitted]"
            if showing and depth > 0 and not _showing(node):
                continue
            if not name and role.lower() not in (
                "entry", "document", "document web"
            ):
                continue
            line = f"{role}: {name[:120]}".rstrip()
            if len(lines) >= max_lines or chars + len(line) + 1 > max_chars:
                truncated = True
                break
            lines.append(line)
            chars += len(line) + 1
        except Exception:
            continue
    if truncated:
        lines.append("… more nodes; use find(query, app=...) or describe(root=node)")
    return "\n".join(lines)


def click(node, *, button: int = 1) -> tuple[int, int]:
    """Click the center of ``node``'s reported extents with real X11 pointer input.

    Use it when an Action call returned true but the application did not react,
    for example a Qt check box whose ``Toggle`` changes its state without the
    click handler. The coordinates come from the node's Component extents. Only
    X11/Xwayland windows receive the click. Returns the clicked desktop point;
    query the tree afterwards to verify the result.
    """
    import subprocess

    import pyatspi

    extents = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
    if extents.width <= 0 or extents.height <= 0:
        raise RuntimeError(f"node has no on-screen extents: {extents}")
    if extents.x == -1 and extents.y == -1:
        raise RuntimeError('Screen origin is unavailable; use the node Action or desktop.screenshot/click')
    x = extents.x + extents.width // 2
    y = extents.y + extents.height // 2
    subprocess.run(["xdotool", "mousemove", "--sync", str(x), str(y), "click", str(button)],
                   check=True, timeout=10)
    return x, y


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True)
    parser.add_argument("--find")
    parser.add_argument("--activate-first", action="store_true")
    args = parser.parse_args()
    if args.find:
        matches = find(args.find, app=args.app)
        for node in matches:
            print(f"{node.getRoleName()}: {node.name}")
        if args.activate_first:
            if not matches:
                raise SystemExit("no matching node")
            actions = matches[0].queryAction()
            names = [actions.getName(index) for index in range(actions.nActions)]
            index = next((index for index, name in enumerate(names)
                          if name in ("click", "press", "activate")), 0)
            print(f"action={names[index]} ok={actions.doAction(index)}")
    else:
        print(describe(app=args.app))
