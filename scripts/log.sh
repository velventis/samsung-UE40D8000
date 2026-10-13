#!/bin/sh
# exeDSP's log on the TV: log.sh [grep -v pattern] (last 200 lines, deduplicated)
cd "$(dirname "$0")/.." && python3 scripts/con.py -t 20 'cat /hd2/log/exedsp.log' | awk '!s[$0]++' | grep -av "${1:-^$}" | tail -${N:-200}
