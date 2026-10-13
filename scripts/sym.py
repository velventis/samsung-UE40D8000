#!/usr/bin/env python3
"""sym.py ADDR... : name the function containing a (prelinked) address in
exeDSP or the TV libraries"""
import bisect, os, subprocess, sys
TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBS = ['sq_exe/exeDSP'] + ['sq_exe/lib/' + f for f in os.listdir(os.path.join(TOP, 'sq_exe/lib')) if '.so' in f]
tabs = []
for l in LIBS:
    p = os.path.join(TOP, l)
    if os.path.islink(p): continue
    out = subprocess.run(['readelf', '-lW', p], capture_output=True, text=True).stdout
    loads = [x.split() for x in out.splitlines() if x.strip().startswith('LOAD')]
    if not loads: continue
    lo = int(loads[0][2], 16); hi = max(int(x[2], 16) + int(x[5], 16) for x in loads)
    tabs.append((lo, hi, l, p))
for a in sys.argv[1:]:
    v = int(a, 16)
    for lo, hi, l, p in tabs:
        if lo <= v < hi:
            syms = []
            for flag in (['-D'], []):
                out = subprocess.run(['nm', '-C', '--defined-only'] + flag + [p], capture_output=True, text=True).stdout
                for x in out.splitlines():
                    f = x.split(' ', 2)
                    if len(f) == 3 and f[1] in 'tTwW':
                        syms.append((int(f[0], 16), f[2]))
            syms.sort()
            i = bisect.bisect_right([s[0] for s in syms], v) - 1
            print('%s: %s %s+0x%x' % (a, os.path.basename(l), syms[i][1] if i >= 0 else '?', v - syms[i][0] if i >= 0 else 0))
            break
    else:
        print('%s: ?' % a)
