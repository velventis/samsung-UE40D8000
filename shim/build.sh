#!/bin/sh
# build libhd2tv.so against the TV's glibc 2.11 (ARMv7 EABI, softfp)
set -e
export TMPDIR=$(cd "$(dirname "$0")/.." && pwd)/work/tmp; mkdir -p $TMPDIR
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
clang --target=armv7a-linux-gnueabi -marm -O2 -static -nostdlib -ffreestanding \
  -fno-stack-protector -fuse-ld=lld -o tools/hd2daemon tools/hd2daemon.c
cp tools/hd2daemon ../overlay/rootfs/sbin/
clang --target=armv7a-linux-gnueabi -marm -O2 -static -nostdlib -ffreestanding \
  -fno-stack-protector -fuse-ld=lld -o tools/memdump tools/memdump.c
cp tools/memdump ../overlay/rootfs/sbin/
# hd2gl: Android (bionic) against the HD2 ROM's Adreno EGL/GLES2, NDK r23
NDK=${NDK:-$HOME/Android/Sdk/ndk/23.1.7779620}
ROM=${HD2_ROM:-$HOME/clones/hd2-qemu/firmware/rom/system}
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi16-clang \
  -O2 -Wall -fno-pie -no-pie -Wl,--hash-style=sysv -o hd2gl/hd2gl hd2gl/hd2gl.c \
  -L$ROM/lib/egl -l:libEGL_adreno200.so -l:libGLESv2_adreno200.so
