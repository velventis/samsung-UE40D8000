#!/usr/bin/env python3
"""Build the HD2 images that boot the Samsung UE40D8000 (Genoa-P) firmware.

  out/nand.img    HD2 NAND: cLK 1.4, boot.img (HD2 kernel from kbuild/),
                  "system" = the TV rootfs (squashfs, patched)
  out/sdcard.img  microSD standing in for the TV's eMMC and OneNAND:
                    p1 swu  p2 rocommon  p3 emanual  p5 contents  (FAT)
                    p6 appext (squashfs)  p7 appext 2nd  p8 rwcommon (FAT)
                    p9 exe (squashfs)  p10 rwarea (FAT)
                  The rootfs's /dev/stl0/13 (exe) and /dev/stl0/12 (rwarea)
                  nodes point at p9 and p10.

usage: scripts/mkimages.py [--only rootfs|exe|nand|sd]
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HD2 = os.path.expanduser('~/clones/hd2-qemu')
sys.path.insert(0, os.path.join(HD2, 'tools'))
import hd2_mknand as mk  # noqa: E402

SQ = os.path.join(TOP, 'tools/squashfs-tools/squashfs-tools')
WORK = os.path.join(TOP, 'work')
OUT = os.path.join(TOP, 'out')
OVERLAY = os.path.join(TOP, 'overlay')
LK = os.path.join(HD2, 'firmware/lk-user.bin')
ZIMAGE = os.path.join(TOP, 'kbuild/arch/arm/boot/zImage')

# HD2 MTD order from cLK 1.4: misc recovery sboot boot system cache userdata
CMDLINE = ('x=/dev/tfsr6 root=/dev/mtdblock4 rootfstype=squashfs ro '
           'console=ttyMSM0,115200n8 init=/sbin/init' +
           (' ' + os.environ['CMDLINE_EXTRA'] if os.environ.get('CMDLINE_EXTRA') else ''))

# rootfs device nodes that change: (major, minor)
DEV_REMAP = {
    'dev/ttyS2': ('c', 204, 64),     # the TV's shell console -> HD2 UART1
    'dev/stl0/12': ('b', 179, 10),   # rwarea
    'dev/stl0/13': ('b', 179, 9),    # exe
}
DEV_ADD = {
    'dev/fb0': ('c', 29, 0),
}

# SD card partitions: (number, MiB, content); content: 'fat' or a path
SD_PARTS = [
    (1, 16, 'fat'), (2, 8, 'fat'), (3, 16, 'fat'),
    (5, 64, 'fat'), (6, None, 'dec/appext.img'), (7, 4, 'fat'),
    (8, 64, 'fat'), (9, None, 'out/exe.img'), (10, 32, 'fat'),
]


def run(*a, **kw):
    return subprocess.run(a, check=True, **kw)


def unsquash(img, dst):
    shutil.rmtree(dst, ignore_errors=True)
    # as a user the device nodes fail; they are recreated from the listing
    subprocess.run([SQ + '/unsquashfs', '-q', '-no-xattrs', '-d', dst, img],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    nodes = {}
    lst = subprocess.run([SQ + '/unsquashfs', '-lln', img], check=True,
                         capture_output=True, text=True).stdout
    for line in lst.splitlines():
        m = re.match(r'([cb])\S+\s+\S+\s+(\d+),\s*(\d+)\s.*? squashfs-root/(.*)$',
                     line)
        if m:
            nodes[m.group(4)] = (m.group(1), int(m.group(2)), int(m.group(3)))
    return nodes


def apply_overlay(name, dst):
    src = os.path.join(OVERLAY, name)
    if os.path.isdir(src):
        run('cp', '-a', '--remove-destination', src + '/.', dst)


def mksquash(src, img, nodes):
    pf = img + '.pseudo'
    with open(pf, 'w') as f:
        for path, (t, maj, mnr) in sorted(nodes.items()):
            if os.path.lexists(os.path.join(src, path)):
                os.unlink(os.path.join(src, path))
            f.write('%s %s 755 0 0 %d %d\n' % (path, t, maj, mnr))
    if os.path.exists(img):
        os.unlink(img)
    run(SQ + '/mksquashfs', src, img, '-all-root', '-no-xattrs', '-comp',
        'gzip', '-b', '131072', '-noappend', '-quiet', '-pf', pf,
        stdout=subprocess.DEVNULL)
    print('%s: %d bytes' % (os.path.relpath(img, TOP), os.path.getsize(img)))


def build_rootfs():
    dst = os.path.join(WORK, 'rootfs')
    nodes = unsquash(os.path.join(TOP, 'dec/rootfs.img'), dst)
    nodes.update(DEV_REMAP)
    nodes.update(DEV_ADD)
    apply_overlay('rootfs', dst)
    mksquash(dst, os.path.join(OUT, 'rootfs.img'), nodes)


def build_exe():
    dst = os.path.join(WORK, 'exe')
    nodes = unsquash(os.path.join(TOP, 'dec/exe.img'), dst)
    apply_overlay('exe', dst)
    mksquash(dst, os.path.join(OUT, 'exe.img'), nodes)


def build_nand():
    lk = bytearray(open(LK, 'rb').read())
    off, parts = mk.clk14_ptable(lk)
    rootfs = open(os.path.join(OUT, 'rootfs.img'), 'rb').read()
    want = {'boot': 5, 'system': (len(rootfs) >> 20) + 2, 'cache': 1}
    for p in parts:
        if p[0] in want and p[2] != want[p[0]]:
            struct.pack_into('<I', lk, p[3], want[p[0]])
            p[2] = want[p[0]]
    layout = {n: (b, s) for n, b, s in mk.clk14_layout(parts)}
    secs, start, count = mk.os_sectors_from_lk(bytes(lk))
    path = os.path.join(OUT, 'nand.img')
    nand = mk.Nand(path)
    mk.write_factory_macs(nand, mk.parse_mac('00:23:76:48:44:32'),
                          mk.parse_mac('00:23:76:48:44:33'))
    for i, s in enumerate(secs):
        nand.write_page(mk.ROM_BLOCK * mk.PPB + i, s)

    def put(name, data):
        b0, size = layout[name]
        if len(data) > size * mk.BLK:
            raise SystemExit('%s: %d bytes do not fit' % (name, len(data)))
        for i in range(0, len(data), mk.PAGE):
            nand.write_page(b0 * mk.PPB + i // mk.PAGE, data[i:i + mk.PAGE])
        print('  %-8s block 0x%04x %5d blocks, %d bytes'
              % (name, b0, size, len(data)))

    boot = mk.mkbootimg(open(ZIMAGE, 'rb').read(), b'', CMDLINE)
    put('boot', boot)
    put('system', rootfs)
    nand.close()
    print('out/nand.img, cmdline "%s"' % CMDLINE)


def build_sd():
    path = os.path.join(OUT, 'sdcard.img')
    sizes = []
    for num, mb, content in SD_PARTS:
        if mb is None:
            mb = (os.path.getsize(os.path.join(TOP, content)) >> 20) + 2
        sizes.append((num, mb, content))
    # MBR: p1-p3 primary, p4 extended holding p5..; 1 MiB alignment
    script = ['label: dos', 'unit: sectors']
    lba = 2048
    starts = {}
    for num, mb, _ in sizes[:3]:
        starts[num] = lba
        script.append('start=%d, size=%d, type=c' % (lba, mb * 2048))
        lba += mb * 2048
    ext_size = sum(mb * 2048 + 2048 for _, mb, _ in sizes[3:])
    script.append('start=%d, size=%d, type=5' % (lba, ext_size))
    lba += 2048
    for num, mb, content in sizes[3:]:
        starts[num] = lba
        t = 'c' if content == 'fat' else '83'
        script.append('start=%d, size=%d, type=%s' % (lba, mb * 2048, t))
        lba += mb * 2048 + 2048
    total = lba
    sig = path + '.layout'
    layout = '\n'.join(script)
    if (os.environ.get('SD_FRESH') != '1' and os.path.exists(path) and
            os.path.exists(sig) and open(sig).read() == layout):
        # same layout: only the squashfs partitions change, the FAT ones
        # (the TV's settings, the EEPROM files) are kept
        for num, mb, content in sizes:
            if content != 'fat':
                run('dd', 'if=' + os.path.join(TOP, content), 'of=' + path,
                    'bs=1M', 'conv=notrunc', 'oflag=seek_bytes',
                    'seek=%d' % (starts[num] * 512), 'status=none')
                print('  p%-2d %4d MiB %s (updated)' % (num, mb, content))
        return
    with open(path, 'wb') as f:
        f.truncate(total * 512)
    run('sfdisk', '-q', path, input='\n'.join(script) + '\n', text=True)
    open(sig, 'w').write(layout)
    tmp = os.path.join(WORK, 'part.img')
    for num, mb, content in sizes:
        if content == 'fat':
            if os.path.exists(tmp):
                os.unlink(tmp)
            run('mkfs.vfat', *(['-F', '32'] if mb >= 64 else []), '-C', tmp,
                str(mb * 1024), stdout=subprocess.DEVNULL)
            data = tmp
        else:
            data = os.path.join(TOP, content)
        run('dd', 'if=' + data, 'of=' + path, 'bs=1M', 'conv=notrunc',
            'oflag=seek_bytes', 'seek=%d' % (starts[num] * 512),
            'status=none')
        print('  p%-2d %4d MiB %s' % (num, mb, content))
    size = 1 << (total * 512 - 1).bit_length()     # QEMU: a power of 2
    with open(path, 'r+b') as f:
        f.truncate(size)
    print('out/sdcard.img: %d MiB' % (size >> 20))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', choices=['rootfs', 'exe', 'nand', 'sd'],
                    action='append')
    a = ap.parse_args()
    steps = a.only or ['rootfs', 'exe', 'nand', 'sd']
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    for s in steps:
        globals()['build_' + s]()


if __name__ == '__main__':
    main()
