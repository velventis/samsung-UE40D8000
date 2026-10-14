#!/bin/sh
# rebuild the shim and images, reboot the emulator, wait
cd "$(dirname "$0")/.." || exit 1
shim/build.sh >/dev/null || exit 1
scripts/stop.sh                 # before touching the images it has open
{ [ -n "$ROCOMMON" ] || [ ! -f out/rocommon.img ]; } &&
    { scripts/mkimages.py --only rocommon >/dev/null || exit 1; }
[ -n "$ROOTFS" ] && { scripts/mkimages.py --only rootfs >/dev/null || exit 1; }
scripts/mkimages.py --only exe --only sd >/dev/null || exit 1
scripts/start.sh 2>/dev/null
sleep ${WAIT:-35}
