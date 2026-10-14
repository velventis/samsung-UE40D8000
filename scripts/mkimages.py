#!/usr/bin/env python3
"""Build the HD2 images that boot the Samsung UE40D8000 (Genoa-P) firmware.

  out/nand.img    HD2 NAND: cLK 1.4, boot.img (HD2 kernel from kbuild/),
                  "system" = the TV rootfs (squashfs, patched)
  out/rocommon.img  /mtd_rocommon: the UE46D5520's (dec/d5520-rocommon.img,
                  from scripts/nand/extract_rocommon.py) with the D8000's
                  bitmap names added, converted from same-named images
  out/sdcard.img  microSD standing in for the TV's eMMC and OneNAND:
                    p1 swu  p3 emanual  p5 contents  (FAT)  p2 rocommon
                    p6 appext (squashfs)  p7 appext 2nd  p8 rwcommon (FAT)
                    p9 exe (squashfs)  p10 rwarea (FAT)
                  The rootfs's /dev/stl0/13 (exe) and /dev/stl0/12 (rwarea)
                  nodes point at p9 and p10.

usage: scripts/mkimages.py [--only rootfs|exe|rocommon|nand|sd]
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
ROM = os.environ.get('HD2_ROM', os.path.join(HD2, 'firmware/rom/system'))
ZIMAGE = os.path.join(TOP, 'kbuild/arch/arm/boot/zImage')

# HD2 MTD order from cLK 1.4: misc recovery sboot boot system cache userdata
# mem=: EBI1 bank 0 (to a 4 MiB boundary: banks before another must end on
# one), and the emulator's RAM between it
# and the PMEM pools (0x3B700000), 183 MiB that the TV's memory needs
CMDLINE = ('x=/dev/tfsr6 root=/dev/mtdblock4 rootfstype=squashfs ro '
           'console=ttyMSM0,115200n8 init=/sbin/init '
           'mem=0x1E400000@0x11800000 mem=0x0B700000@0x30000000' +
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
    (1, 16, 'fat'), (2, None, 'out/rocommon.img'), (3, 16, 'fat'),
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


def add_android(dst):
    """hd2/android/system: what hd2gl (the TV's GL on the Adreno 200) needs
    from the HD2 ROM: bionic's linker, the Adreno EGL/GLES2 with what they
    link, and KGSL's firmware"""
    sysd = os.path.join(dst, 'hd2/android/system')
    shutil.rmtree(sysd, ignore_errors=True)
    files = {'bin/linker', 'etc/firmware/yamato_pm4.fw', 'etc/firmware/yamato_pfp.fw'}
    todo = ['lib/egl/libEGL_adreno200.so', 'lib/egl/libGLESv2_adreno200.so',
            'lib/egl/eglsubAndroid.so', 'lib/libsc-a2xx.so']
    while todo:
        f = todo.pop()
        if f in files:
            continue
        files.add(f)
        out = subprocess.run(['llvm-readelf', '-d', os.path.join(ROM, f)],
                             check=True, capture_output=True, text=True).stdout
        for n in re.findall(r'\(NEEDED\).*\[(.*)\]', out):
            for d in ('lib/', 'lib/egl/'):
                if os.path.exists(os.path.join(ROM, d + n)):
                    todo.append(d + n)
                    break
    for f in sorted(files):
        os.makedirs(os.path.join(sysd, os.path.dirname(f)), exist_ok=True)
        shutil.copyfile(os.path.join(ROM, f), os.path.join(sysd, f))
        os.chmod(os.path.join(sysd, f), 0o755 if f.startswith('bin/') else 0o644)
    shutil.copyfile(os.path.join(TOP, 'shim/hd2gl/hd2gl'), os.path.join(sysd, 'bin/hd2gl'))
    os.chmod(os.path.join(sysd, 'bin/hd2gl'), 0o755)
    print('exe: %d files from the HD2 ROM, and hd2gl' % len(files))


def build_exe():
    dst = os.path.join(WORK, 'exe')
    nodes = unsquash(os.path.join(TOP, 'dec/exe.img'), dst)
    apply_overlay('exe', dst)
    add_android(dst)
    mksquash(dst, os.path.join(OUT, 'exe.img'), nodes)


def build_rocommon():
    """the D5520's rocommon, plus the images the D8000's programs name that
    it has under another extension or in another folder"""
    from PIL import Image
    dst = os.path.join(WORK, 'rocommon')
    unsquash(os.path.join(TOP, 'dec/d5520-rocommon.img'), dst)
    imgdir = os.path.join(dst, 'Images_960x540')
    exe = os.path.join(WORK, 'exe')
    if not os.path.isdir(exe):
        unsquash(os.path.join(TOP, 'dec/exe.img'), exe)
    wanted = set()
    for root, _, files in os.walk(exe):
        for f in files:
            if f == 'exeDSP' or f.endswith('.so') or '.so.' in f:
                out = subprocess.run(['strings', '-n', '6', os.path.join(root, f)],
                                     capture_output=True, text=True).stdout
                for w in out.splitlines():
                    w = w.split('Images_960x540/')[-1]
                    if re.fullmatch(r'[\w\- ]+(/[\w\- ]+)*\.(bmp|png)', w):
                        wanted.add(w)
    have = {}                       # lower-case stem -> [relative paths]
    for root, _, files in os.walk(imgdir):
        for f in files:
            stem, ext = os.path.splitext(f)
            if ext.lower() in ('.png', '.bmp', '.jpg'):
                rel = os.path.relpath(os.path.join(root, f), imgdir)
                have.setdefault(stem.lower(), []).append(rel)
    made = 0
    for w in sorted(wanted):
        if os.path.exists(os.path.join(imgdir, w)):
            continue
        stem = os.path.splitext(os.path.basename(w))[0].lower()
        cands = have.get(stem)
        if not cands:
            continue
        # the same folder, else Common, else any
        wdir = os.path.dirname(w).lower()
        cands = sorted(cands, key=lambda c: (os.path.dirname(c).lower() != wdir,
                                             not c.startswith('Common'), c))
        im = Image.open(os.path.join(imgdir, cands[0])).convert('RGBA')
        out = os.path.join(imgdir, w)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        im.save(out, 'BMP' if w.endswith('.bmp') else 'PNG')
        made += 1
    print('rocommon: %d of %d wanted images added' % (made, len(wanted)))
    mksquash(dst, os.path.join(OUT, 'rocommon.img'), {})


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
    for num, mb, content in sizes[:3]:
        starts[num] = lba
        t = 'c' if content == 'fat' else '83'
        script.append('start=%d, size=%d, type=%s' % (lba, mb * 2048, t))
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
    # a new layout: FAT partitions that keep their number and size are
    # carried over from the old card (unless SD_FRESH=1)
    keep = {}
    if os.environ.get('SD_FRESH') != '1' and os.path.exists(path):
        import json
        pt = json.loads(subprocess.run(['sfdisk', '-J', path], check=True,
                                       capture_output=True, text=True).stdout)
        old = {int(re.search(r'(\d+)$', p['node']).group(1)): p
               for p in pt['partitiontable']['partitions']}
        for num, mb, content in sizes:
            o = old.get(num)
            if content == 'fat' and o and o['type'] == 'c' and o['size'] == mb * 2048:
                keep[num] = o['start']
        if keep:
            os.replace(path, path + '.old')
    with open(path, 'wb') as f:
        f.truncate(total * 512)
    run('sfdisk', '-q', path, input='\n'.join(script) + '\n', text=True)
    open(sig, 'w').write(layout)
    tmp = os.path.join(WORK, 'part.img')
    for num, mb, content in sizes:
        if num in keep:
            run('dd', 'if=' + path + '.old', 'of=' + path, 'bs=1M',
                'conv=notrunc', 'iflag=skip_bytes,count_bytes',
                'oflag=seek_bytes', 'skip=%d' % (keep[num] * 512),
                'count=%d' % (mb << 20), 'seek=%d' % (starts[num] * 512),
                'status=none')
            print('  p%-2d %4d MiB fat (kept)' % (num, mb))
            continue
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
    if keep:
        os.unlink(path + '.old')
    print('out/sdcard.img: %d MiB' % (size >> 20))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', choices=['rootfs', 'exe', 'rocommon', 'nand', 'sd'],
                    action='append')
    a = ap.parse_args()
    steps = a.only or ['rootfs', 'exe', 'rocommon', 'nand', 'sd']
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    for s in steps:
        globals()['build_' + s]()


if __name__ == '__main__':
    main()
