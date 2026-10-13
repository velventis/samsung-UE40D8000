#!/bin/sh
# exeDSP's threads: tid, state, wchan, user pc (symbolized)
cd "$(dirname "$0")/.." || exit 1
adb -s 127.0.0.1:${ADB_PORT:-5575} shell 'p=$(pidof exeDSP); for t in /proc/$p/task/*; do
  set -- $(cat $t/stat); echo "${t##*/} $3 $(cat $t/wchan) ${30} ${14} ${15}"; done' | tr -d '\r' > run/threads.txt
pcs=$(awk '{printf "%x ", $4}' run/threads.txt)
python3 scripts/sym.py $pcs > run/threads.sym
paste -d' ' run/threads.txt run/threads.sym | awk '{printf "%-6s %s %-22s utime %-6s %s %s\n", $1, $2, $3, $5, $8, $9}' | sort -k6 | ${FILTER:-cat}
