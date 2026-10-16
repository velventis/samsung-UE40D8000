#!/bin/sh
# Run in the virt guest after mounting the helper disk on /mnt.
set -e
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t tmpfs -o size=256M tmpfs /tmp
mount -t tmpfs -o size=512M tmpfs /hd2
mount -t tmpfs -o size=32M tmpfs /dtv
mount -t tmpfs -o size=16M tmpfs /dsm
mount -t tmpfs tmpfs /core
mkdir -p /dev/sam /dev/pts /dev/mqueue
mount -t devpts devpts /dev/pts
mount -t mqueue mqueue /dev/mqueue
mount -t squashfs -o ro /dev/vda9 /mtd_exe
mount -t squashfs -o ro /dev/vda2 /mtd_rocommon
mount -t squashfs -o ro /dev/vda6 /mtd_appext
mount -t vfat /dev/vda10 /mtd_rwarea
mount -t vfat /dev/vda8 /mtd_rwcommon
mount -t vfat /dev/vda5 /mtd_contents
mount -t vfat /dev/vda1 /mtd_swu
mount -t vfat /dev/vda3 /mtd_emanual
mkdir -p /hd2/log
ln -s /mtd_exe/.info /dtv/.info
# Firmware UARTs are PTYs supplied by the shim. Keep PL011 for this shell.
mount -o bind /dev /mnt/linux/dev
mount -o bind /proc /mnt/linux/proc
mount -o bind /sys /mnt/linux/sys
mount -t tmpfs tmpfs /mnt/linux/tmp
cat /proc/version
LD_LIBRARY_PATH=/lib:/usr/lib:/mtd_exe/Comp_LIB:/mtd_exe/lib \
 /lib/ld-linux.so.3 --list /mtd_exe/exeDSP
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 MESA_SHADER_CACHE_DISABLE=true LP_NUM_THREADS=2 chroot /mnt/linux /usr/bin/tvgl --selftest
echo 'Mesa setup complete. Start with: sh /mnt/run-exedsp.sh'
