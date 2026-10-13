#!/bin/sh
# stop only this checkout's emulator
cd "$(dirname "$0")/../run" && [ -f qemu.pid ] && kill $(cat qemu.pid) 2>/dev/null; rm -f qemu.pid
