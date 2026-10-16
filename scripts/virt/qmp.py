#!/usr/bin/env python3
"""Send one QMP command to this project's virt guest."""
import json
import os
from pathlib import Path
import socket
import sys
p = Path(os.environ.get('TV_PROJECT', str(Path.home() / 'samsung-UE40D8000'))) / 'work/virt-mesa'
s = socket.socket(socket.AF_UNIX)
s.settimeout(10)
s.connect(str(p / 'qmp.sock'))
f = s.makefile('rwb')
json.loads(f.readline())
for name in ['qmp_capabilities', sys.argv[1] if len(sys.argv) > 1 else 'query-status']:
    f.write((json.dumps({'execute': name}) + '\n').encode()); f.flush()
    while True:
        line = f.readline()
        if not line: break
        r = json.loads(line)
        if 'error' in r: raise RuntimeError(r)
        if 'return' in r:
            print(json.dumps(r['return']))
            break
s.close()
