#!/usr/bin/env python3
"""Package the Samsung TV firmware for a real HTC HD2 booted by cLK 1.5.

  scripts/mkhd2zip.py [-o out/hd2-samsung-ue40d8000.zip]

The zip holds:
  boot.img            the HD2 kernel (kbuild/) and its command line
  boot-extramem.img   the same, also using RAM at 0x30000000-0x3B6FFFFF (see
                      README: unverified on a real HD2)
  system.img          the TV's root filesystem (out/rootfs.img, SquashFS)
  sdcard.img          the microSD card (out/sdcard.img): the TV's eMMC and
                      OneNAND partitions
  flash.sh, flash.bat cLK 1.5 fastboot: the partition layout, boot, system
  README.txt

Build first: scripts/build-kernel.sh, scripts/mkimages.py (all steps).
"""
import argparse
import os
import sys
import zipfile

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HD2 = os.path.expanduser('~/clones/hd2-qemu')
sys.path.insert(0, os.path.join(HD2, 'tools'))
import hd2_mknand as mk  # noqa: E402

BASE_CMDLINE = ('x=/dev/tfsr6 root=/dev/mtdblock4 rootfstype=squashfs ro '
                'console=ttyMSM0,115200n8 init=/sbin/init')
EXTRA_MEM = ' mem=0x1E400000@0x11800000 mem=0x0B700000@0x30000000'

# cLK 1.5 partitions, in order: the kernel finds the TV's root at mtdblock4
LAYOUT = [('misc', 1), ('recovery', 5), ('sboot', 5), ('boot', 5),
          ('system', None), ('cache', 5), ('userdata', 0)]

README = """\
Samsung UE40D8000 (2011) TV firmware on the HTC HD2
====================================================

The TV's own software (Samsung's exeDSP with the original SDAL libraries)
on the HD2's Linux kernel, with libhd2tv standing in for the TV's hardware.
The UI is drawn by the HD2's Adreno 200 and shown rotated on its screen.

This has only run in an HD2 emulator (hd2-qemu), never on a real HD2.

What you need
-------------
- An HD2 with HSPL and cLK 1.5 (LEOIMG.nbh flashed with DAF.exe).
- fastboot (Android platform-tools) on your computer.
- A microSD card of at least 1 GB. ALL ITS DATA WILL BE LOST.
- This replaces what is on the HD2's NAND (Android, recovery). Back up first.

1. The microSD card
-------------------
Write sdcard.img to the card (whole card, not a partition):
  Linux:   sudo dd if=sdcard.img of=/dev/sdX bs=4M conv=fsync
  Windows: balenaEtcher or Win32 Disk Imager
Put the card in the HD2.

2. The NAND
-----------
Start the HD2 into cLK (hold Back while powering on) and choose FASTBOOT;
connect USB. Then run flash.sh (Linux/macOS) or flash.bat (Windows). It:
  - replaces cLK's partition table with
      misc 1, recovery 5, sboot 5, boot 5, system {system_mb}, cache 5,
      userdata (the rest) MiB
    and commits it (the HD2 restarts into cLK: choose FASTBOOT again when
    the script asks),
  - flashes boot.img and system.img and reboots.

3. First boot
-------------
The first boot takes several minutes (the TV writes its factory defaults),
then shows the TV's "No signal" message. With no TV hardware there is no
picture source; Smart Hub works offline (its home screen).

Remote control: over USB with adb (the TV system runs adbd):
  adb shell 'echo smarthub > /hd2/rckey'
Keys: smarthub, menu, source, up, down, left, right, enter, return, exit,
tools, info, guide, red, green, yellow, blue, 0-9, chup, chdown, volup,
voldown, mute, or a Samsung key code (e.g. 0x79).
Logs: adb pull /hd2/log/exedsp.log

Memory
------
boot.img uses the HD2 kernel's usual memory map. In the emulator, Smart Hub
also needed the RAM between 0x30000000 and 0x3B700000 (boot-extramem.img).
Whether a real HD2 has RAM there is not known: try boot-extramem.img with
  fastboot flash boot boot-extramem.img
only knowing it may not boot (reflash boot.img then).

Back to Android
---------------
In cLK fastboot: fastboot oem part-create-default, then install your ROM.
"""


def fastboot_script(win, system_mb):
    lines = []
    say = (lambda s: 'echo %s' % s) if win else (lambda s: "echo '%s'" % s)
    lines.append('@echo off' if win else '#!/bin/sh\nset -e\ncd "$(dirname "$0")"')
    lines.append(say('Partition table (cLK 1.5)'))
    lines.append('fastboot oem part-clear')
    for name, mb in LAYOUT:
        lines.append('fastboot oem part-add %s:%d' % (name, system_mb if mb is None else mb))
    lines.append('fastboot oem part-list')
    lines.append('fastboot oem part-commit')
    if win:
        lines.append('echo The HD2 restarts: choose FASTBOOT in cLK again, then')
        lines.append('pause')
    else:
        lines.append(say('The HD2 restarts: choose FASTBOOT in cLK again, then press Enter'))
        lines.append('read dummy')
    lines.append('fastboot flash boot boot.img')
    lines.append('fastboot flash system system.img')
    lines.append('fastboot reboot')
    return ('\r\n' if win else '\n').join(lines) + ('\r\n' if win else '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', default=os.path.join(TOP, 'out/hd2-samsung-ue40d8000.zip'))
    a = ap.parse_args()
    zimage = open(os.path.join(TOP, 'kbuild/arch/arm/boot/zImage'), 'rb').read()
    rootfs = open(os.path.join(TOP, 'out/rootfs.img'), 'rb').read()
    # whole NAND pages; cLK takes an image whose size is a multiple of
    # page + spare (2112) for a YAFFS one: pad by a page if it is one
    system = rootfs + b'\0' * (-len(rootfs) % 2048)
    if len(system) % 2112 == 0:
        system += b'\0' * 2048
    system_mb = (len(system) >> 20) + 2
    files = {
        'boot.img': mk.mkbootimg(zimage, b'', BASE_CMDLINE),
        'boot-extramem.img': mk.mkbootimg(zimage, b'', BASE_CMDLINE + EXTRA_MEM),
        'system.img': system,
        'flash.sh': fastboot_script(False, system_mb).encode(),
        'flash.bat': fastboot_script(True, system_mb).encode(),
        'README.txt': README.format(system_mb=system_mb).encode(),
    }
    with zipfile.ZipFile(a.o, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, data in files.items():
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 14, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if name == 'flash.sh' else 0o644) << 16
            z.writestr(info, data)
        z.write(os.path.join(TOP, 'out/sdcard.img'), 'sdcard.img')
    print('%s: %d MiB' % (os.path.relpath(a.o, TOP), os.path.getsize(a.o) >> 20))


if __name__ == '__main__':
    main()
