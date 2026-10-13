#!/bin/sh
# Build the HD2 kernel (src: kernel/, branch samsung-tv) for the TV userland.
set -e
cd "$(dirname "$0")/.."
TC=$HOME/clones/hd2-qemu/toolchains/arm-eabi-4.8/bin/arm-eabi-
M="make -C kernel O=$PWD/kbuild ARCH=arm CROSS_COMPILE=$TC KCFLAGS=-mno-unaligned-access"
if [ ! -f kbuild/.config ]; then
    $M htcleo_defconfig
    cat >>kbuild/.config <<'CFG'
CONFIG_SYSVIPC=y
CONFIG_POSIX_MQUEUE=y
CONFIG_SQUASHFS=y
# CONFIG_SQUASHFS_EMBEDDED is not set
CONFIG_SERIAL_MSM=y
CONFIG_SERIAL_MSM_CONSOLE=y
# CONFIG_ANDROID_PARANOID_NETWORK is not set
CFG
    $M oldconfig </dev/null >kbuild/oldconfig.log 2>&1 || true
fi
ps -eLo comm | grep -qx GameThread && { echo "game running, not building"; exit 1; }
$M -j${JOBS:-16} zImage 2>&1 | tee kbuild/build.log | grep -E 'error|Error|zImage' || true
ls -la kbuild/arch/arm/boot/zImage
