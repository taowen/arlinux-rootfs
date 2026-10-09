#!/usr/bin/env python3
"""Linux and hosted Android desktop entries in one searchable app drawer."""
import gi
gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, Gio, Gtk

# Helpers that are installed with desktop entries but are not applications a user opens.
HIDDEN = {
    "arlinux-apps.desktop",
    "foot-server.desktop", "footclient.desktop", "thunar-bulk-rename.desktop", "thunar-settings.desktop",
    "org.xfce.mousepad-settings.desktop", "sniff.desktop",
}
SECTIONS = (("linux", "Linux apps"), ("android", "Android apps"))
CSS = b"""
window { background: #13161b; color: #eaf0f6; }
.title { font-size: 22px; font-weight: 700; }
.count { color: #8f9aa6; font-size: 14px; }
.section { color: #8f9aa6; font-size: 13px; font-weight: 700; letter-spacing: 1px; margin: 18px 6px 6px 6px; }
entry { background: #1f242c; color: #eaf0f6; border: 1px solid #2b323c; border-radius: 20px; padding: 8px 14px; min-height: 24px; }
entry:focus { border-color: #6edebe; }
.filter { background: #1f242c; color: #c9d2db; border: none; border-radius: 18px; padding: 6px 18px; box-shadow: none; }
.filter:checked { background: #6edebe; color: #081016; font-weight: 700; }
flowboxchild { border-radius: 16px; padding: 10px 4px; }
flowboxchild:hover { background: #1f242c; }
flowboxchild:selected, flowboxchild:active { background: #26313a; }
.tile-name { font-size: 13px; color: #dfe6ed; }
.android-tag { color: #6edebe; font-size: 11px; }
.empty { color: #8f9aa6; font-size: 15px; margin: 40px; }
"""


def kind(app):
    command = app.get_commandline() or ""
    return "android" if app.get_id().startswith("arlinux-android-") or command.startswith("arlinux-app ") else "linux"


def entries():
    apps = [app for app in Gio.AppInfo.get_all()
            if app.should_show() and app.get_id() not in HIDDEN and "Settings" not in (app.get_categories() or "").split(";")]
    return sorted(apps, key=lambda app: app.get_display_name().casefold())


def icon(app):
    """The entry's icon, or a generic one when the theme does not provide it."""
    gicon = app.get_icon()
    theme = Gtk.IconTheme.get_default()
    if isinstance(gicon, Gio.ThemedIcon) and any(theme.has_icon(name) for name in gicon.get_names()):
        return gicon
    if isinstance(gicon, Gio.FileIcon) and gicon.get_file().query_exists():
        return gicon
    return Gio.ThemedIcon.new("application-x-executable")


def tile(app):
    child = Gtk.FlowBoxChild()
    child.app = app
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
    box.set_size_request(112, -1)  # equal tiles, so one long name cannot widen every column
    image = Gtk.Image.new_from_gicon(icon(app), Gtk.IconSize.DIALOG)
    image.set_pixel_size(56)
    box.pack_start(image, False, False, 0)
    # One ellipsized line keeps every tile's natural width equal (the full name is the tooltip).
    name = Gtk.Label(label=app.get_display_name(), justify=Gtk.Justification.CENTER)
    name.set_width_chars(11)
    name.set_max_width_chars(11)
    name.set_ellipsize(3)  # Pango.EllipsizeMode.END
    name.get_style_context().add_class("tile-name")
    box.pack_start(name, False, False, 0)
    child.add(box)
    child.set_tooltip_text(app.get_description() or app.get_display_name())
    return child


def activate(application):
    if application.get_windows():
        application.get_windows()[0].present()
        return
    provider = Gtk.CssProvider()
    provider.load_from_data(CSS)
    Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    window = Gtk.ApplicationWindow(application=application, title="Apps")
    window.set_default_size(980, 680)
    window.connect("destroy", lambda _: application.quit())
    window.connect("key-press-event", lambda _, event: event.keyval == Gdk.KEY_Escape and window.destroy())
    outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
    outer.set_border_width(20)
    window.add(outer)

    apps = entries()
    header = Gtk.Box(spacing=12)
    title = Gtk.Label(label="Apps", xalign=0)
    title.get_style_context().add_class("title")
    header.pack_start(title, False, False, 0)
    counts = {key: sum(kind(app) == key for app in apps) for key, _ in SECTIONS}
    count = Gtk.Label(label=f"{counts['linux']} Linux · {counts['android']} Android", xalign=0)
    count.get_style_context().add_class("count")
    header.pack_start(count, False, False, 0)
    filters, group = {}, None
    for key, text in (("android", "Android"), ("linux", "Linux"), ("all", "All")):
        button = Gtk.RadioButton.new_with_label_from_widget(group, text)
        button.set_mode(False)
        button.get_style_context().add_class("filter")
        group = group or button
        filters[key] = button
        header.pack_end(button, False, False, 0)
    filters["all"].set_active(True)
    outer.pack_start(header, False, False, 0)

    search = Gtk.SearchEntry()
    search.set_placeholder_text("Search Linux and Android apps")
    outer.pack_start(search, False, False, 0)

    scroller = Gtk.ScrolledWindow()
    scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
    outer.pack_start(scroller, True, True, 0)
    body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
    scroller.add(body)

    def launch(_, child):
        try:
            child.app.launch([], Gio.AppLaunchContext())
        except Exception as error:
            dialog = Gtk.MessageDialog(transient_for=window, modal=True,
                message_type=Gtk.MessageType.ERROR, buttons=Gtk.ButtonsType.CLOSE, text=str(error))
            dialog.run()
            dialog.destroy()
            return
        window.destroy()

    sections = []
    for key, text in SECTIONS:
        heading = Gtk.Label(label=text.upper(), xalign=0)
        heading.get_style_context().add_class("section")
        grid = Gtk.FlowBox(homogeneous=True, selection_mode=Gtk.SelectionMode.NONE, column_spacing=6, row_spacing=6)
        grid.set_min_children_per_line(4)
        grid.set_max_children_per_line(10)
        grid.set_activate_on_single_click(True)
        grid.connect("child-activated", launch)
        for app in apps:
            if kind(app) == key:
                grid.add(tile(app))
        body.pack_start(heading, False, False, 0)
        body.pack_start(grid, False, False, 0)
        sections.append((key, heading, grid))
    empty = Gtk.Label(label="No matching apps")
    empty.get_style_context().add_class("empty")
    body.pack_start(empty, False, False, 0)

    def refresh(*_):
        query = search.get_text().casefold()
        selected = next(key for key, button in filters.items() if button.get_active())
        total = 0
        for key, heading, grid in sections:
            shown = 0
            for child in grid.get_children():
                visible = query in child.app.get_display_name().casefold()
                child.set_visible(visible)
                shown += visible
            on = selected in ("all", key) and shown > 0
            heading.set_visible(on)
            grid.set_visible(on)
            total += shown if on else 0
        empty.set_visible(total == 0)

    search.connect("search-changed", refresh)
    for button in filters.values():
        button.connect("toggled", refresh)
    window.show_all()
    refresh()
    search.grab_focus()


application = Gtk.Application(application_id="org.arlinux.Applications")
application.connect("activate", activate)
application.run([])
