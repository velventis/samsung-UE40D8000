#!/bin/sh
# stop only this checkout's emulator (and wait: it unlinks its sockets on exit)
cd "$(dirname "$0")/../run" || exit 0
[ -f qemu.pid ] || exit 0
p=$(cat qemu.pid); rm -f qemu.pid
kill $p 2>/dev/null
for i in $(seq 50); do kill -0 $p 2>/dev/null || exit 0; sleep 0.1; done
kill -9 $p 2>/dev/null
