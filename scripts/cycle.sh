#!/bin/sh
# rebuild the shim and exe image, reboot the emulator, wait for exeDSP
cd "$(dirname "$0")/.." || exit 1
shim/build.sh >/dev/null || exit 1
scripts/mkimages.py --only exe --only sd >/dev/null || exit 1
[ -n "$ROOTFS" ] && { scripts/mkimages.py --only rootfs --only nand >/dev/null || exit 1; }
scripts/stop.sh; scripts/start.sh 2>/dev/null
sleep ${WAIT:-35}
