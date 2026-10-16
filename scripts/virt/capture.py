#!/usr/bin/env python3
from pathlib import Path
import json
import socket
import os
import sys
from PIL import Image

p = Path(os.environ.get('TV_PROJECT', str(Path.home() / 'samsung-UE40D8000'))) / 'work/virt-mesa'
s = socket.socket(socket.AF_UNIX)
s.connect(str(p / 'qmp.sock'))
f = s.makefile('rwb')
json.loads(f.readline())
def cmd(name, **args):
    f.write((json.dumps({'execute': name, 'arguments': args}) + '\n').encode()); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'error' in r: raise RuntimeError(r)
        if 'return' in r: return r['return']
cmd('qmp_capabilities')
cmd('screendump', filename=str(p / 'screen.ppm'))
out = Path(sys.argv[1]) if len(sys.argv) > 1 else p / 'screen.png'
im = Image.open(p / 'screen.ppm')
im.save(out)
print('Captured:', im.size, 'nonblack bounds:', im.getbbox(), '->', out)
s.close()
