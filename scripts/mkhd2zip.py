#!/usr/bin/env python3
"""Package the Samsung TV firmware for a real HTC HD2 booted by cLK.

  scripts/mkhd2zip.py [-o out/hd2-samsung-ue40d8000.zip]

The zip holds:
  boot.img            the HD2 kernel (kbuild/) and its command line
  boot-extramem.img   the same, also using RAM at 0x30000000-0x3B6FFFFF (see
                      README: unverified on a real HD2)
  sdcard.img          the microSD card (out/sdcard.img): the TV's eMMC and
                      OneNAND partitions, and its root filesystem (p11)
  flash.sh, flash.bat fastboot: boot.img to cLK's boot partition
  README.txt

Only the boot partition is written, so any cLK partition layout works (1.4
included, which cannot repartition over fastboot).

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

BASE_CMDLINE = ('x=/dev/tfsr6 root=/dev/mmcblk0p11 rootwait rootfstype=squashfs ro '
                'console=ttyMSM0,115200n8 init=/sbin/init')
EXTRA_MEM = ' mem=0x1E400000@0x11800000 mem=0x0B700000@0x30000000'

README = """\
Samsung UE40D8000 (2011) TV firmware on the HTC HD2
====================================================

The TV's own software (Samsung's exeDSP with the original SDAL libraries)
on the HD2's Linux kernel, with libhd2tv standing in for the TV's hardware.
The UI is drawn by the HD2's Adreno 200 and shown rotated on its screen.

This has only run in an HD2 emulator (hd2-qemu), never on a real HD2.

What you need
-------------
- An HD2 with HSPL and cLK (1.4 or later; LEOIMG.nbh flashed with DAF.exe).
- fastboot (Android platform-tools) on your computer.
- A microSD card of at least 1 GB. ALL ITS DATA WILL BE LOST.
- Only cLK's boot partition is replaced (the kernel of the Android you have
  installed): your system and data partitions are left alone.

1. The microSD card
-------------------
Write sdcard.img to the card (whole card, not a partition):
  Linux:   sudo dd if=sdcard.img of=/dev/sdX bs=4M conv=fsync
  Windows: balenaEtcher or Win32 Disk Imager
Put the card in the HD2. It holds everything but the kernel, the TV's root
filesystem included.

2. The kernel
-------------
Start the HD2 into cLK (hold Back while powering on) and choose FASTBOOT;
connect USB. Then run flash.sh (Linux/macOS) or flash.bat (Windows), or:
  fastboot flash boot boot.img
  fastboot reboot

3. First boot
-------------
The first boot takes several minutes (the TV writes its factory defaults),
then shows the TV's "No signal" message. With no TV hardware there is no
picture source; Smart Hub works offline (its home screen).

The HD2's buttons: the Windows button opens Smart Hub, the volume rocker
is the TV's volume (hold to repeat).

More remote keys over USB with adb (the TV system runs adbd):
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
Flash your ROM's boot.img again (fastboot flash boot boot.img, or reinstall
the ROM from recovery), and put the SD card you use with Android back.
"""


def fastboot_script(win):
    if win:
        return '@echo off\r\nfastboot flash boot boot.img\r\nfastboot reboot\r\n'
    return ('#!/bin/sh\nset -e\ncd "$(dirname "$0")"\n'
            'fastboot flash boot boot.img\nfastboot reboot\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', default=os.path.join(TOP, 'out/hd2-samsung-ue40d8000.zip'))
    a = ap.parse_args()
    zimage = open(os.path.join(TOP, 'kbuild/arch/arm/boot/zImage'), 'rb').read()
    files = {
        'boot.img': mk.mkbootimg(zimage, b'', BASE_CMDLINE),
        'boot-extramem.img': mk.mkbootimg(zimage, b'', BASE_CMDLINE + EXTRA_MEM),
        'flash.sh': fastboot_script(False).encode(),
        'flash.bat': fastboot_script(True).encode(),
        'README.txt': README.encode(),
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
