#!/bin/sh
# Samsung userspace on virt, with Linux/Mesa rendering and no Android/KGSL.
set -e
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export LD_LIBRARY_PATH=/lib:/usr/lib
. /mnt/tv-environment.sh
export MTD_SWU=/dev/vda1 MTD_ROCOMMON=/dev/vda2 MTD_EMANUAL=/dev/vda3
export MTD_CONTENTS=/dev/vda5 MTD_APPEXT0=/dev/vda6 MTD_APPEXT1=/dev/vda7
export MTD_RWCOMMON=/dev/vda8 MTD_RWAREA_STL=/dev/vda10 MTD_EXE_0_STL=/dev/vda9
export HD2TV_LOG=${HD2TV_LOG:-1} HD2TV_FPS=${HD2TV_FPS:-10}
export HD2TV_GL_ROOT=${HD2TV_GL_ROOT:-/mnt/linux}
export HD2TV_GL_SOFTWARE=${HD2TV_GL_SOFTWARE:-1}
export HD2TV_GL_DRIVER=${HD2TV_GL_DRIVER:-softpipe}
if pidof exeDSP >/dev/null; then
    echo "exeDSP is already running; stop that test before starting another." >&2
    exit 1
fi
cd /mtd_exe
echo 30000 > /mtd_rwarea/DelayValue.txt
touch /hd2/stdin
# A fresh inode prevents old firmware children from creating holes in a new log.
rm -f /hd2/log/exedsp.log
# The pipe holds stdin open without providing input, as exeDSP requires.
tail -f /hd2/stdin | LD_PRELOAD=/mnt/libhd2tv.so ./exeDSP > /hd2/log/exedsp.log 2>&1 &
echo $! > /hd2/exedsp.pid
echo "exeDSP launched; inspect /hd2/log/exedsp.log"
