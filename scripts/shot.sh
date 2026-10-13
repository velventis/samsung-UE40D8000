#!/bin/sh
# screenshot of the emulated HD2 screen: shot.sh [out.png]
cd "$(dirname "$0")/.." || exit 1
O=${1:-run/screen.png}
python3 ~/clones/hd2-qemu/tools/qmon.py run/qmp.sock "screendump $PWD/run/screen.ppm" >/dev/null &&
python3 -c "
from PIL import Image; Image.open('run/screen.ppm').save('$O')" && echo $O
