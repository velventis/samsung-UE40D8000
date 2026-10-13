#!/bin/sh
# Boot the Samsung TV firmware on the emulated HD2 (headless, background).
#   console: UART1 (the TV's ttyS2 shell) on run/console.sock, logged to
#   run/uart1.log; `scripts/con.py` talks to it.
cd "$(dirname "$0")/../run" || exit 1
HD2=$HOME/clones/hd2-qemu
rm -f qmp.sock console.sock ramcon.log qemu.log uart1.log
../bin/qemu-tv -M htcleo -drive if=mtd,format=raw,file=${NAND:-../out/nand.img} \
  -drive if=sd,format=raw,file=${SDCARD:-../out/sdcard.img} \
  -display ${DISPLAY_OPT:-none} \
  -chardev socket,id=con,path=console.sock,server=on,wait=off,logfile=uart1.log \
  -serial chardev:con -serial file:ramcon.log \
  -qmp unix:qmp.sock,server,nowait -d ${QEMU_D:-guest_errors} -D qemu.log \
  -audiodev none,id=snd0 -global msm-modem.audiodev=snd0 \
  -nic user,model=bcm4329 \
  -chardev socket,id=adb,host=127.0.0.1,port=${ADB_PORT:-5575},server=on,wait=off -global msm-hsusb.adb=adb "$@" > qemu.out 2>&1 &
echo $! > qemu.pid
for i in $(seq 20); do [ -S qmp.sock ] && break; sleep 0.3; done
sleep 2.5
python3 $HD2/tools/keys.py qmp.sock f2
