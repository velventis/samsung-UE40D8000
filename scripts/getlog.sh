#!/bin/sh
# pull exeDSP's log to run/exedsp.log
cd "$(dirname "$0")/.." && adb -s 127.0.0.1:${ADB_PORT:-5575} pull /hd2/log/exedsp.log run/exedsp.log >/dev/null && grep -av '^\s*$' run/exedsp.log > run/exedsp.txt && wc -l < run/exedsp.txt
