#!/usr/bin/env python3
"""Stands in for zenity / kdialog in plat's file-dialog fallback test
(plat-selftest-linux-services.sh links it as `zenity` on a private PATH).

Behaves like the real tool from plat's side: takes the same argv, prints the
chosen paths on stdout (--separator for zenity --multiple, one per line for
kdialog --separate-output), exits 1 on cancel. The answer comes from the
fake desktop's control object (TakeFileChooserResponse, which also logs our
argv), set by plat's fileDialogRespond test hook.
"""
import os
import sys
import urllib.parse

import dbus

argv = sys.argv[:]
argv[0] = os.path.basename(argv[0])
try:
    control = dbus.SessionBus().get_object("org.nisdos.PlatTest", "/org/nisdos/PlatTest", introspect=False)
    uris = control.TakeFileChooserResponse(argv, dbus_interface="org.nisdos.PlatTest", timeout=60)
except dbus.exceptions.DBusException as e:
    print("fake dialog:", e, file=sys.stderr)
    sys.exit(2)

paths = [urllib.parse.unquote(u[len("file://"):]) for u in uris if u.startswith("file://")]
if not paths:
    sys.exit(1)  # cancelled
sep = "\n"
for a in argv:
    if a.startswith("--separator="):
        sep = a[len("--separator="):]
sys.stdout.write(sep.join(paths) + "\n")
