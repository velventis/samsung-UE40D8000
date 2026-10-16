#!/bin/bash
# Keep the TV ABI in the shim; modern ARMhf Mesa runs in a separate process.
set -euo pipefail
cd "$(dirname "$0")/.."
BASE=$PWD/work/virt-mesa
RUNTIME=$BASE/root
mkdir -p "$BASE/bin" work/tmp
export TMPDIR=$PWD/work/tmp
L=$PWD/work/exe/lib
clang --target=armv7a-linux-gnueabi -mfloat-abi=softfp -mfpu=vfpv3 -marm -mtp=cp15 \
  -O2 -g -fPIC -ffreestanding -fno-builtin -fno-stack-protector -Wall \
  -Wno-unused-function -shared -nostdlib -fuse-ld=lld -Wl,-soname,libhd2tv.so \
  -Wl,--no-undefined -o "$BASE/bin/libhd2tv.so" shim/*.c \
  "$L/libc-2.11.1.so" "$L/libdl-2.11.1.so" "$L/libpthread-2.11.1.so" "$L/libgcc_s.so.1"
arm-linux-gnueabihf-gcc -O2 -g -Wall -funwind-tables -rdynamic -D_FILE_OFFSET_BITS=64 \
  -I"$RUNTIME/usr/include" -o "$BASE/bin/tvgl" shim/hd2gl/hd2gl.c \
  -L"$RUNTIME/usr/lib/arm-linux-gnueabihf" \
  -Wl,-rpath-link,"$RUNTIME/usr/lib/arm-linux-gnueabihf" -lEGL -lGLESv2
cp "$BASE/bin/tvgl" "$RUNTIME/usr/bin/tvgl"
printf '%s\n' 'Built shim and Linux/Mesa renderer under work/virt-mesa/bin'
