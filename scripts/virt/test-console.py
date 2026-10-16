#!/usr/bin/env python3
"""Exercise serial completion with empty output, CRLF and split socket reads."""
from pathlib import Path
import os, re, socket, subprocess, tempfile, threading, time
script = Path(__file__).with_name('console.py')
for mode in ('first-line', 'crlf', 'fragmented'):
    with tempfile.TemporaryDirectory() as project:
        base = Path(project) / 'work/virt-mesa'
        base.mkdir(parents=True)
        server = socket.socket(socket.AF_UNIX)
        server.bind(str(base / 'console.sock')); server.listen(1)
        def serve():
            conn, _ = server.accept()
            with conn:
                data = b''
                while not re.search(rb'__VIRT_DONE_[0-9]+__', data):
                    data += conn.recv(4096)
                marker = re.search(rb'__VIRT_DONE_[0-9]+__', data).group()
                if mode == 'first-line': conn.sendall(marker + b'\nVDLinux#> ')
                elif mode == 'crlf': conn.sendall(b'value\r\n' + marker + b'\r\n')
                else:
                    conn.sendall(marker[:8]); time.sleep(.02)
                    conn.sendall(marker[8:] + b'\n')
        thread = threading.Thread(target=serve); thread.start()
        env = os.environ.copy(); env['TV_PROJECT'] = project
        result = subprocess.run(['python3', str(script), 'true', '--timeout', '2'],
                                env=env, capture_output=True, text=True)
        thread.join(); server.close()
        assert result.returncode == 0, (mode, result.stdout, result.stderr)
        print('Console completion passed:', mode)
