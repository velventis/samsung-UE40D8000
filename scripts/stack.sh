#!/bin/sh
# stack.sh TID: return addresses on an exeDSP thread's stack (from /proc)
cd "$(dirname "$0")/.." || exit 1
T=$1
adb -s 127.0.0.1:${ADB_PORT:-5575} shell "p=\$(pidof exeDSP); set -- \$(cat /proc/\$p/task/$T/stat); sp=\${29}; echo pc \${30} sp \$sp; /hd2/memdump \$p \$sp 2048 > /hd2/stk.bin" | tr -d '\r'
adb -s 127.0.0.1:${ADB_PORT:-5575} pull /hd2/stk.bin run/stk.bin >/dev/null 2>&1
python3 - <<'PY' | xargs python3 scripts/sym.py | grep -v ': ?' | awk '!s[$0]++' | head -${N:-25}
import struct
d = open('run/stk.bin', 'rb').read()
for i in range(0, len(d) - 3, 4):
    v = struct.unpack_from('<I', d, i)[0]
    if 0x8000 <= v < 0x7460000 or 0x41000000 <= v < 0x42000000:
        print('%x' % v)
PY
