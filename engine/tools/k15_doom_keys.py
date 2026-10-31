#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Play DOOM on the FiiO K15 from your PC keyboard.

Opens a small window; while it has focus, key presses/releases are sent as UDP packets
(2 bytes: pressed, doom keycode) to doomk15 on the K15, port 6666.

Usage: python k15_doom_keys.py [k15-ip]     (default: $K15_HOST)
Works with plain Windows Python or WSL (needs tkinter).
"""
import os
import socket
import sys
import tkinter as tk

HOST = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("K15_HOST")
if not HOST:
    sys.exit("usage: k15_doom_keys.py <k15-ip>  (or set K15_HOST)")
PORT = 6666

# doomkeys.h values
K = {
    "Up": 0xAD, "Down": 0xAF, "Left": 0xAC, "Right": 0xAE,
    "Control_L": 0xA3, "Control_R": 0xA3,           # fire
    "space": 0xA2,                                   # use
    "Shift_L": 0x80 + 0x36, "Shift_R": 0x80 + 0x36,  # run
    "Alt_L": 0x80 + 0x38, "Alt_R": 0x80 + 0x38,      # strafe modifier
    "Return": 13, "KP_Enter": 13, "Escape": 27, "Tab": 9, "BackSpace": 0x7F,
    "minus": 0x2D, "equal": 0x3D, "comma": ord(","), "period": ord("."),
    "Pause": 0xFF,
}
for i in range(12):
    K[f"F{i + 1}"] = 0x80 + (0x3B + i if i < 10 else 0x57 + i - 10)
# WASD-style aliases for people who prefer them
ALIAS = {"w": "Up", "s": "Down", "a": ",", "d": "."}

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
held = set()
pending_release = {}


def code_for(ev):
    sym = ev.keysym
    if sym in K:
        return K[sym]
    if len(sym) == 1:
        sym = sym.lower()
        if sym in ALIAS:
            a = ALIAS[sym]
            return K.get(a, ord(a) if len(a) == 1 else None)
        if sym.isalnum():
            return ord(sym)
    return None


def send(pressed, code):
    sock.sendto(bytes([1 if pressed else 0, code]), (HOST, PORT))


def on_press(ev):
    code = code_for(ev)
    if code is None:
        return
    # X11 auto-repeat sends release+press pairs; cancel the pending release instead
    job = pending_release.pop(code, None)
    if job:
        root.after_cancel(job)
    if code not in held:
        held.add(code)
        send(True, code)
        status.set(f"down {ev.keysym}")


def on_release(ev):
    code = code_for(ev)
    if code is None:
        return

    def really_release():
        pending_release.pop(code, None)
        if code in held:
            held.discard(code)
            send(False, code)
            status.set(f"up {ev.keysym}")

    pending_release[code] = root.after(30, really_release)


def on_focus_out(_ev):
    for code in list(held):
        send(False, code)
    held.clear()


root = tk.Tk()
root.title(f"K15 DOOM -> {HOST}:{PORT}")
tk.Label(
    root,
    justify="left",
    font=("Courier", 11),
    text=(
        "Keep this window focused.\n\n"
        "Arrows / W S + A D(strafe)  move\n"
        "Ctrl  fire     Space  use\n"
        "Shift run      Alt    strafe\n"
        "1-7   weapons  Tab    map\n"
        "Esc   menu     Enter  select   y/n confirm"
    ),
).pack(padx=16, pady=12)
status = tk.StringVar(value="ready")
tk.Label(root, textvariable=status).pack(pady=(0, 10))
root.bind("<KeyPress>", on_press)
root.bind("<KeyRelease>", on_release)
root.bind("<FocusOut>", on_focus_out)
root.mainloop()
