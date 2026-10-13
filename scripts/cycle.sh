#!/bin/sh
# rebuild the shim and images, reboot the emulator, wait
cd "$(dirname "$0")/.." || exit 1
shim/build.sh >/dev/null || exit 1
scripts/stop.sh                 # before touching the images it has open
scripts/mkimages.py --only exe --only sd >/dev/null || exit 1
[ -n "$ROOTFS" ] && { scripts/mkimages.py --only rootfs --only nand >/dev/null || exit 1; }
scripts/start.sh 2>/dev/null
sleep ${WAIT:-35}
