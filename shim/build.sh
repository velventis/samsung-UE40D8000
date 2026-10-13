#!/bin/sh
# build libhd2tv.so against the TV's glibc 2.11 (ARMv7 EABI, softfp)
set -e
cd "$(dirname "$0")"
L=../work/exe/lib
clang --target=armv7a-linux-gnueabi -mfloat-abi=softfp -mfpu=vfpv3 -marm \
  -O2 -g -fPIC -ffreestanding -fno-builtin -fno-stack-protector -Wall \
  -Wno-unused-function -shared -nostdlib -fuse-ld=lld -Wl,-soname,libhd2tv.so \
  -Wl,--no-undefined -o libhd2tv.so *.c \
  $L/libc-2.11.1.so $L/libdl-2.11.1.so $L/libpthread-2.11.1.so $L/libgcc_s.so.1
mkdir -p ../overlay/exe/hd2
cp libhd2tv.so ../overlay/exe/hd2/
echo built libhd2tv.so
