#!/usr/bin/env python3
"""Send a command to the nice!nano lightbar controller over USB serial.

Usage: lightbar.py on | up [n] | down [n] | cool [n] | warm [n] | reset | bri N | ct N
       lightbar.py id HEX | info | pair | sniff
"""
import glob
import sys
import time

import serial

ports = glob.glob("/dev/cu.usbmodem*")
if not ports:
    sys.exit("board not found (no /dev/cu.usbmodem*)")

cmd = " ".join(sys.argv[1:]) or "info"
sniff = cmd == "sniff"
with serial.Serial(ports[0], 115200, timeout=1.5) as s:
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    deadline = time.time() + (120 if sniff else 5)
    try:
        while time.time() < deadline:
            line = s.readline().decode(errors="replace").strip()
            if not line:
                if not sniff:
                    break  # no more output
                continue
            print(line)
            if "learned" in line:
                break
    finally:
        if sniff:
            s.write(b"x\n")  # stop sniff mode
