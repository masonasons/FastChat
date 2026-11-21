#!/usr/bin/env python3
"""A small GTK window for the Linux desktop tests (computer use).

It has a Name field and a "Press me" button, and prints its X window id when it
is up. On standard output: "xid <id>", then "pressed" for each press of the
button. A line on standard input asks for the field's text ("text?" -> "text
<text>"); "quit" or the end of input closes it.
"""
import sys

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("GdkX11", "3.0")
from gi.repository import GdkX11, GLib, Gtk  # noqa: E402,F401


def say(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


window = Gtk.Window(title="FastChat test window")
window.set_default_size(360, 160)
window.set_focus_on_map(False)  # the tests act through accessibility, not the keyboard
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=10)
label = Gtk.Label(label="Name", xalign=0)
entry = Gtk.Entry()
label.set_mnemonic_widget(entry)
button = Gtk.Button(label="Press me")
button.connect("clicked", lambda _b: say("pressed"))
for w in (label, entry, button):
    box.pack_start(w, False, False, 0)
window.add(box)
window.connect("destroy", Gtk.main_quit)
window.show_all()


def announce():
    say("xid %d" % window.get_window().get_xid())
    return False


def on_stdin(channel, _condition):
    line = channel.readline()
    if not line or line.strip() == "quit":
        Gtk.main_quit()
        return False
    if line.strip() == "text?":
        say("text " + entry.get_text())
    return True


GLib.timeout_add(300, announce)
GLib.io_add_watch(GLib.IOChannel.unix_new(sys.stdin.fileno()), GLib.IO_IN | GLib.IO_HUP, on_stdin)
Gtk.main()
