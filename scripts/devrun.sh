#!/bin/sh
# (re)start exeDSP on a running TV (booted with CMDLINE_EXTRA=hd2noexe) with
# a freshly built shim pushed over adb.  Log: /hd2/log/exedsp.log
cd "$(dirname "$0")/.." || exit 1
A="adb -s 127.0.0.1:${ADB_PORT:-5575}"
scripts/adbc.sh || exit 1
shim/build.sh >/dev/null || exit 1
sed -e '/hd2noexe/d' -e 's|/mtd_exe/hd2/libhd2tv.so|/hd2/libhd2tv.so|' \
    -e "1a export HD2TV_LOG=${HD2TV_LOG:-0x25}" overlay/exe/rc.local > work/run.sh
$A shell 'killall -9 exeDSP 2>/dev/null; sleep 1' 
$A push shim/libhd2tv.so /hd2/libhd2tv.so >/dev/null && $A push work/run.sh /hd2/run.sh >/dev/null || exit 1
$A shell 'mkdir -p /hd2/log; chmod 755 /hd2/run.sh; cd /mtd_exe; /sbin/hd2daemon /bin/sh -c "sh -x /hd2/run.sh > /hd2/log/run.out 2>&1"; sleep 2'
sleep ${WAIT:-20}
