#!/bin/sh
# (re)connect adb to the TV; prints nothing on success
P=127.0.0.1:${ADB_PORT:-5575}
for i in 1 2 3 4 5 6; do
  adb -s $P shell true >/dev/null 2>&1 && exit 0
  adb disconnect $P >/dev/null 2>&1; adb connect $P >/dev/null 2>&1; sleep 2
done
echo "adb: no connection to $P"; exit 1
