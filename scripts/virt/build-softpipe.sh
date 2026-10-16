#!/bin/bash
# Build a small softpipe runtime without installing packages on the WSL host.
set -euo pipefail
cd "${TV_PROJECT:-$HOME/samsung-UE40D8000}"
BASE=$PWD/work/virt-mesa
BUILD=$BASE/mesa-build
mkdir -p "$BUILD"/{host-debs,host-tools,python,arm-dev}
SOURCE=$BUILD/mesa_25.2.8.orig.tar.xz
if [ ! -f "$SOURCE" ]; then
 curl -fL https://ports.ubuntu.com/ubuntu-ports/pool/main/m/mesa/mesa_25.2.8.orig.tar.xz -o "$SOURCE"
fi
echo "097842f3e49d996868b38688db87b006f7d4541e93ce86d2f341d8b3e7be7c93  $SOURCE" | sha256sum -c -
if [ ! -d "$BUILD/mesa-25.2.8" ]; then tar -xJf "$SOURCE" -C "$BUILD"; fi
if patch -d "$BUILD/mesa-25.2.8" -p1 --forward --dry-run < scripts/virt/mesa-lazy-texture-cache.patch >/dev/null; then
 patch -d "$BUILD/mesa-25.2.8" -p1 < scripts/virt/mesa-lazy-texture-cache.patch
else
 patch -d "$BUILD/mesa-25.2.8" -p1 --reverse --dry-run < scripts/virt/mesa-lazy-texture-cache.patch >/dev/null
fi
python3 -m pip install --target "$BUILD/python" --upgrade \
 meson==1.12.1 Mako==1.4.3 MarkupSafe==3.0.4 packaging==26.3 PyYAML==6.0.3
(
 cd "$BUILD/host-debs"
 apt-get download g++-13-arm-linux-gnueabihf libstdc++-13-dev-armhf-cross libstdc++6-armhf-cross
 for pkg in *.deb; do dpkg-deb -x "$pkg" "$BUILD/host-tools"; done
)
# Reuse the signed ARM package indexes created by prepare-mesa-runtime.sh.
OPTS=(-o "Dir::Etc::sourcelist=$BASE/apt/sources.list" -o Dir::Etc::sourceparts=-
 -o "Dir::State::lists=$BASE/apt/lists" -o "Dir::Cache::archives=$BASE/apt/archives"
 -o "Dir::State::status=$BASE/apt/status" -o "Dir::Log=$BASE/apt/log"
 -o "Dir::Cache::pkgcache=$BASE/apt/pkgcache.bin" -o "Dir::Cache::srcpkgcache=$BASE/apt/srcpkgcache.bin"
 -o APT::Architecture=armhf -o APT::Architectures::=armhf
 -o Acquire::Languages=none -o Debug::NoLocking=1)
apt-get "${OPTS[@]}" -y --download-only --no-install-recommends install libexpat1-dev zlib1g-dev libdrm-dev libglvnd-dev
for pkg in "$BASE"/apt/archives/*.deb; do dpkg-deb -x "$pkg" "$BUILD/arm-dev"; done
cat > "$BUILD/armhf.ini" <<CROSS
[binaries]
c = 'arm-linux-gnueabihf-gcc'
cpp = ['$BUILD/host-tools/usr/bin/arm-linux-gnueabihf-g++-13', '-B/usr/arm-linux-gnueabihf/bin/', '-B/usr/lib/gcc-cross/arm-linux-gnueabihf/13/']
ar = 'arm-linux-gnueabihf-ar'
strip = 'arm-linux-gnueabihf-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'
[properties]
sys_root = '$BUILD/arm-dev'
pkg_config_libdir = ['$BUILD/arm-dev/usr/lib/arm-linux-gnueabihf/pkgconfig', '$BUILD/arm-dev/usr/share/pkgconfig']
[built-in options]
cpp_args = ['-isystem', '/usr/arm-linux-gnueabihf/include', '-isystem', '$BUILD/arm-dev/usr/include/arm-linux-gnueabihf']
cpp_link_args = ['-B/usr/arm-linux-gnueabihf/lib/', '-L/usr/arm-linux-gnueabihf/lib', '-B/usr/lib/gcc-cross/arm-linux-gnueabihf/13/', '-L$BUILD/host-tools/usr/arm-linux-gnueabihf/lib']
CROSS
export PYTHONPATH=$BUILD/python
SETUP=(setup)
if [ -f "$BUILD/build/meson-private/coredata.dat" ]; then SETUP+=(--reconfigure); fi
python3 -m mesonbuild.mesonmain "${SETUP[@]}" "$BUILD/build" "$BUILD/mesa-25.2.8" --cross-file "$BUILD/armhf.ini" \
 --prefix=/usr --libdir=lib/arm-linux-gnueabihf --buildtype=release \
 -Dgallium-drivers=softpipe -Dvulkan-drivers=[] -Dplatforms=[] \
 -Dllvm=disabled -Degl=enabled -Dgles1=disabled -Dgles2=enabled \
 -Dopengl=true -Dglx=disabled -Dgbm=disabled -Dglvnd=enabled \
 -Dvideo-codecs=[] -Dbuild-tests=false
ninja -C "$BUILD/build" -j"${TV_BUILD_JOBS:-4}"
python3 -m mesonbuild.mesonmain install -C "$BUILD/build" --destdir "$BUILD/stage" --no-rebuild
# Do not change libraries that a running guest may be using.
python3 - "$BASE/qmp.sock" <<'CHECK'
import socket, sys
s = socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except (FileNotFoundError, ConnectionRefusedError): pass
else: raise SystemExit('Stop the test VM with python3 scripts/virt/qmp.py quit before installing the runtime.')
finally: s.close()
CHECK
if [ ! -d "$BUILD/runtime-before-custom" ]; then cp -a "$BASE/root" "$BUILD/runtime-before-custom"; fi
cp -a "$BUILD/stage/usr/." "$BASE/root/usr/"
echo 'Patched softpipe installed in the isolated runtime. Rebuild the helper image by restarting start-mesa.sh.'
