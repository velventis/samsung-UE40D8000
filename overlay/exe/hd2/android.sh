#!/bin/sh
# The Adreno 200 for the TV's GL (shim gl.c -> hd2gl): a chroot at
# /hd2/android with the HD2 ROM's /system (Android EGL/GLES and bionic,
# from /mtd_exe/hd2/android/system), the GPU's device nodes and /proc;
# KGSL's firmware comes through the kernel hotplug helper.
R=/hd2/android
[ -e $R/system/bin/linker ] && exit 0
echo /mtd_exe/hd2/hotplug.sh > /proc/sys/kernel/hotplug
mkdir -p $R/system $R/dev/log $R/proc $R/data/local/tmp
mount -o bind /mtd_exe/hd2/android/system $R/system
mount -t proc none $R/proc
mknod $R/dev/null c 1 3
mknod $R/dev/kgsl-3d0 c $(sed 's/:/ /' /sys/class/kgsl/kgsl-3d0/dev)
for d in genlock ashmem pmem; do
    mknod $R/dev/$d c 10 $(sed 's/.*://' /sys/class/misc/$d/dev)
done
for d in main system radio events; do
    mknod $R/dev/log/$d c 10 $(sed 's/.*://' /sys/class/misc/log_$d/dev)
done
chmod 666 $R/dev/*
