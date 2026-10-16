#!/usr/bin/env python3
"""Run one command through this checkout's serial socket and save its output."""
import argparse
import pathlib
from pathlib import Path
import socket
import os
import time

p = argparse.ArgumentParser()
p.add_argument('command')
p.add_argument('--timeout', type=float, default=20)
p.add_argument('--output')
a = p.parse_args()
base = pathlib.Path(os.environ.get('TV_PROJECT', str(Path.home() / 'samsung-UE40D8000'))) / 'work/virt-mesa'
marker = '__VIRT_DONE_%d__' % time.time_ns()
s = socket.socket(socket.AF_UNIX)
s.settimeout(1)
s.connect(str(base / 'console.sock'))
# Disable terminal echo before using a completion marker. A wrapped echo of
# the command can otherwise look exactly like the marker's output line.
s.sendall(b'stty -echo\n')
s.settimeout(0.25)
drain_until = time.monotonic() + 2
while time.monotonic() < drain_until:
    try:
        if not s.recv(65536):
            break
    except socket.timeout:
        break
s.settimeout(1)
s.sendall((a.command + "; echo; echo " + marker + '\n').encode())
end = time.monotonic() + a.timeout
data = bytearray()
while time.monotonic() < end:
    try:
        chunk = s.recv(65536)
    except socket.timeout:
        continue
    if not chunk:
        break
    data.extend(chunk)
    # Match the actual marker line, not its terminal echo in the command.
    if marker.encode() in data.replace(b'\r', b'').splitlines():
        break
s.close()
if a.output:
    pathlib.Path(a.output).write_bytes(data)
print(data.decode(errors='replace').replace('\x00', ''))
if marker.encode() not in data.replace(b'\r', b'').splitlines():
    raise SystemExit('serial command did not finish before timeout')
