#!/usr/bin/env python3
"""Rebuild the rocommon SquashFS (Images_960x540, ...) from a raw NAND dump
of a 2011 Samsung TV (UE46D5520RB: 2 GiB MLC, Samsung FSR STL).

  extract_rocommon.py DUMP OUT.img

The dump is raw: pages of 8192 data + 436 spare bytes, 128 pages a block,
with the controller's BCH ECC unapplied (nandecc.c corrects it).  The
FSRPART table near the end lists the STL partitions; the one holding the
SquashFS keeps its logical units (1 MiB) in physical blocks in no
particular order.  They are put back in order from the filesystem itself:
the superblock's unit comes first, the unit with the inode/fragment tables
last, and every other unit goes where the compressed blocks the inodes say
start in it decompress.  Units without compressed blocks are placed by the
zlib streams crossing into them from their neighbours.
"""
import os
import re
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sqmeta  # noqa: E402

RAW_BLOCK = 1104384
PAGE, PAGES = 8192, 128
OPS = PAGE + 96                          # nandecc output: data + OOB
UNIT = PAGE * PAGES
HERE = os.path.dirname(os.path.abspath(__file__))


def nandecc(dump, out, first, n):
    tmp = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'work', 'tmp')
    os.makedirs(tmp, exist_ok=True)
    exe = os.path.join(tmp, 'nandecc')
    subprocess.run(['cc', '-O2', '-o', exe, os.path.join(HERE, 'nandecc.c')],
                   check=True)
    r = subprocess.run([exe, dump, out, str(first), str(n)], check=True,
                       capture_output=True, text=True)
    print(r.stderr.strip().splitlines()[-1])


def fsrpart(dump):
    """[(id, first block, blocks)] from the FSRPART table"""
    size = os.path.getsize(dump)
    with open(dump, 'rb') as f:
        f.seek(size - 4 * RAW_BLOCK)
        tail = f.read()
    i = tail.find(b'FSRPART\0')
    n = struct.unpack_from('<I', tail, i + 12)[0]
    parts = []
    for k in range(n):
        pid, attr, first, cnt = struct.unpack_from('<HHHH', tail, i + 16 + 16 * k)
        parts.append((pid, first, cnt))
    return parts


def main():
    dump, out = sys.argv[1], sys.argv[2]
    work = out + '.ecc'
    parts = fsrpart(dump)
    print('STL partitions:', ', '.join('0x%x: %d+%d' % p for p in parts))
    # the SquashFS lives in the partition whose units start with "hsqs"
    with open(dump, 'rb') as f:
        def sq_in(first, cnt):
            for b in range(first, first + cnt):
                f.seek(b * RAW_BLOCK)
                if f.read(4) == b'hsqs':
                    return True
            return False
        pid, first, cnt = next(p for p in parts if sq_in(p[1], p[2]))
    print('SquashFS in partition 0x%x' % pid)
    nandecc(dump, work, first, cnt)

    units = {}
    with open(work, 'rb') as f:
        for b in range(cnt):
            f.seek(b * PAGES * OPS)
            d = f.read(PAGES * OPS)
            pages = [d[p * OPS:p * OPS + PAGE] for p in range(PAGES)]
            used = [p for p in range(PAGES) if pages[p] != b'\xff' * PAGE or
                    d[p * OPS + PAGE:(p + 1) * OPS] != b'\xff' * 96]
            if not used:
                continue
            # data pages carry their page number in OOB slot 0 (bytes 4..7
            # or 8..11: n, ~n) and slot 1 types 0x1001/0x2001/0x2004
            oob = d[used[0] * OPS + PAGE:used[0] * OPS + PAGE + 24]
            if oob[12] not in (0x10, 0x20):
                continue                    # STL meta unit
            units[first + b] = b''.join(pages)
    os.unlink(work)

    sb_unit = next(b for b, u in units.items() if u[:4] == b'hsqs')
    sb = sqmeta.superblock(units[sb_unit])
    nunits = (sb['bytes_used'] + UNIT - 1) // UNIT
    print('superblock in block %d: %d inodes, %d bytes, %d units'
          % (sb_unit, sb['inodes'], sb['bytes_used'], nunits))

    # the tables: try units as the last one until the metadata parses
    img = bytearray(nunits * UNIT)
    img[:UNIT] = units[sb_unit]
    last = nunits - 1
    blocks = None
    for b, u in units.items():
        img[last * UNIT:(last + 1) * UNIT] = u
        try:
            blocks = sqmeta.blocks(bytes(img), sb)
            break
        except Exception:
            continue
    if blocks is None:
        raise SystemExit('no unit holds the tables')
    tables_unit = b
    print('tables in block %d; %d data/fragment blocks' % (b, len(blocks)))

    # place units by the compressed blocks that must start in them
    order = {0: sb_unit, last: tables_unit}
    free = {b: u for b, u in units.items() if b not in (sb_unit, tables_unit)}
    for k in range(1, last):
        tests = [(s - k * UNIT, ln) for s, ln, c in blocks
                 if c and k * UNIT <= s and s + ln <= (k + 1) * UNIT][:12]
        if not tests:
            continue
        best = []
        for b, u in free.items():
            ok = 0
            for off, ln in tests:
                try:
                    zlib.decompress(u[off:off + ln])
                    ok += 1
                except zlib.error:
                    pass
            if ok == len(tests):
                best.append(b)
        # identical copies (stale and current) are interchangeable
        best = [b for i, b in enumerate(best)
                if all(free[b] != free[c] for c in best[:i])]
        if len(best) == 1:
            order[k] = best[0]
            del free[best[0]]
    # stale copies of placed units
    placed = [units[b] for b in order.values()]
    free = {b: u for b, u in free.items() if u not in placed}

    # the rest: by the compressed blocks that cross into them from the unit
    # before or out of them into the unit after
    def crossing(k, u):
        ok = None
        for s, ln, c in blocks:
            if not c:
                continue
            if s < k * UNIT < s + ln and k - 1 in order:
                prev = units[order[k - 1]]
                buf = (prev[s - (k - 1) * UNIT:] + u)[:ln]
            elif s < (k + 1) * UNIT < s + ln and k + 1 in order:
                buf = (u[s - k * UNIT:] + units[order[k + 1]])[:ln]
            else:
                continue
            try:
                zlib.decompress(buf)
                ok = True
            except zlib.error:
                return False
        return ok

    progress = True
    while progress:
        progress = False
        for k in range(1, last):
            if k in order:
                continue
            fit = [b for b, u in free.items() if crossing(k, u)]
            if len(fit) == 1:
                order[k] = fit[0]
                del free[fit[0]]
                progress = True
    left = [k for k in range(nunits) if k not in order]
    if len(left) == 1 and len(free) == 1:
        order[left[0]] = next(iter(free))
    missing = [k for k in range(nunits) if k not in order]
    if missing:
        raise SystemExit('units not placed: %s' % missing)
    with open(out, 'wb') as f:
        for k in range(nunits):
            f.write(units[order[k]])
        f.truncate((sb['bytes_used'] + 4095) & ~4095)
    print('%s: units %s' % (out, ' '.join(str(order[k]) for k in range(nunits))))


if __name__ == '__main__':
    main()
