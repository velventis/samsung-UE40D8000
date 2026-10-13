#!/usr/bin/env python3
"""Run a shell command on the TV console (UART1, run/console.sock).

usage: con.py [-t SECONDS] 'command'   -> prints its output
"""
import os
import socket
import sys
import time

t = 10.0
args = sys.argv[1:]
if args[:1] == ['-t']:
    t = float(args[1])
    args = args[2:]
cmd = ' '.join(args)
s = socket.socket(socket.AF_UNIX)
s.connect(os.path.join(os.path.dirname(__file__), '../run/console.sock'))
s.settimeout(0.2)
mark = '__END%d__' % (time.time() * 1000 % 100000)
s.sendall(('%s; echo %s\n' % (cmd, mark)).encode())
buf = b''
end = time.time() + t
while time.time() < end:
    try:
        d = s.recv(65536)
    except socket.timeout:
        continue
    if not d:
        break
    buf += d
    txt = buf.decode('latin-1')
    # the echo of the command line contains "echo MARK"; wait for the bare one
    if ('\n' + mark) in txt.replace('\r', ''):
        break
txt = buf.decode('latin-1').replace('\r', '')
lines = txt.split('\n')
out = [l for l in lines[1:] if mark not in l]
print('\n'.join(out).rstrip())
