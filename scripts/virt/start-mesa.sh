#!/bin/bash
set -euo pipefail
cd "${TV_PROJECT:-$HOME/samsung-UE40D8000}"
BASE=$PWD/work/virt-mesa
HELPER=$BASE/helper
case "${TV_SIZE:-1920x1080}" in
  1920x1080) WIDTH=1920; HEIGHT=1080 ;;
  1280x720) WIDTH=1280; HEIGHT=720 ;;
  *) echo 'TV_SIZE must be 1920x1080 or 1280x720' >&2; exit 1 ;;
esac
# Refuse before touching an image that a running guest could have open.
python3 - "$BASE/qmp.sock" <<'CHECK'
import pathlib, socket, sys
path = pathlib.Path(sys.argv[1])
if path.exists():
    s = socket.socket(socket.AF_UNIX)
    try:
        s.connect(str(path))
    except (ConnectionRefusedError, FileNotFoundError):
        path.unlink(missing_ok=True)
    else:
        raise SystemExit("The virt test VM is already running. Stop it with python3 scripts/virt/qmp.py quit first.")
    finally:
        s.close()
CHECK
mkdir -p "$HELPER"
if [ ! -f "$BASE/bin/tvgl" ] || [ ! -f "$BASE/root/usr/lib/arm-linux-gnueabihf/libEGL.so.1" ]; then
  echo 'Build the isolated Mesa runtime and run bash shim/build-linux.sh first.' >&2
  exit 1
fi
# Refresh new runtime files too, including libraries from a custom Mesa build.
python3 - "$BASE/root" "$HELPER/linux" <<'SYNC'
from pathlib import Path
import os, sys
source, target = map(Path, sys.argv[1:])
target.mkdir(parents=True, exist_ok=True)
for item in source.rglob('*'):
    dest = target / item.relative_to(source)
    if item.is_symlink():
        value = os.readlink(item)
        if dest.is_symlink() and os.readlink(dest) == value: continue
        if dest.exists() or dest.is_symlink(): dest.unlink()
        dest.symlink_to(value)
    elif item.is_dir(): dest.mkdir(parents=True, exist_ok=True)
    else:
        if dest.exists() and os.path.samefile(item, dest): continue
        if dest.exists() or dest.is_symlink(): dest.unlink()
        os.link(item, dest)
SYNC
cp "$BASE/bin/libhd2tv.so" "$HELPER/libhd2tv.so"
cp scripts/virt/guest-setup.sh "$HELPER/prepare.sh"
cp scripts/virt/run-exedsp.sh "$HELPER/run-exedsp.sh"
grep '^export ' overlay/exe/rc.local > "$HELPER/tv-environment.sh"
./tools/squashfs-tools/squashfs-tools/mksquashfs "$HELPER" "$BASE/helper.sqfs" \
  -noappend -comp gzip -no-xattrs -all-root -quiet
exec "${TV_QEMU:-./bin/qemu-tv}" \
  -M virt,highmem=off -cpu cortex-a15 -m "${TV_RAM:-2048}" -smp 1 \
  -kernel "${TV_KERNEL:-$HOME/linux-6.12.113/arch/arm/boot/zImage}" \
  -append "root=/dev/vda11 rootfstype=squashfs rootwait ro console=ttyAMA0,115200 init=/bin/sh video=Virtual-1:${WIDTH}x${HEIGHT}@60" \
  -drive if=none,id=tvdisk,file=out/sdcard.img,format=raw,snapshot=on \
  -device virtio-blk-pci,drive=tvdisk,addr=0x1 \
  -drive if=none,id=helper,file="$BASE/helper.sqfs",format=raw,readonly=on \
  -device virtio-blk-pci,drive=helper,addr=0x2 \
  -device "virtio-gpu-pci,addr=0x3,xres=$WIDTH,yres=$HEIGHT" \
  -chardev socket,id=con,path="$BASE/console.sock",server=on,wait=off,logfile="$BASE/console.log" \
  -serial chardev:con -display "${TV_DISPLAY:-sdl}" -monitor none -nic none \
  -qmp unix:"$BASE/qmp.sock",server=on,wait=off \
  -D "$BASE/qemu.log" -d guest_errors
