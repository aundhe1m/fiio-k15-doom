#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Send a sequence of DOOM keys to doomk15 over UDP. Usage: udpkeys.py <key> [<key> ...]

Keys: esc enter up down left right fire use tab, or any single character.
"""
import os
import socket
import sys
import time

HOST = os.environ.get("K15_HOST")
if not HOST:
    sys.exit("set K15_HOST to the K15's IP address")
NAMES = {"esc": 27, "enter": 13, "up": 0xAD, "down": 0xAF, "left": 0xAC, "right": 0xAE,
         "fire": 0xA3, "use": 0xA2, "tab": 9}

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for k in sys.argv[1:]:
    code = NAMES.get(k, ord(k[0]))
    s.sendto(bytes([1, code]), (HOST, 6666))
    time.sleep(0.12)
    s.sendto(bytes([0, code]), (HOST, 6666))
    time.sleep(0.3)
