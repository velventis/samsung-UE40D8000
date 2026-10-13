#!/bin/sh
# kernel hotplug helper: answers firmware requests (the Adreno's
# yamato_pm4.fw and yamato_pfp.fw, for KGSL) from the HD2 ROM's files
FW=/mtd_exe/hd2/android/system/etc/firmware
[ "$ACTION" = add ] && [ -n "$FIRMWARE" ] || exit 0
d=/sys$DEVPATH
[ -e $d/loading ] || exit 0
if [ -f $FW/$FIRMWARE ]; then
    echo 1 > $d/loading
    cat $FW/$FIRMWARE > $d/data
    echo 0 > $d/loading
else
    echo -1 > $d/loading
fi
