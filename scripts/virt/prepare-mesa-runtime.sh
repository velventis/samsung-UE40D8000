#!/bin/bash
# Download/extract an isolated ARMhf Mesa runtime; never installs host packages.
set -euo pipefail
cd "${TV_PROJECT:-$HOME/samsung-UE40D8000}"
BASE=$PWD/work/virt-mesa
mkdir -p "$BASE"/{apt/lists/partial,apt/archives/partial,apt/log,root}
touch "$BASE/apt/status"
cat > "$BASE/apt/sources.list" <<'SOURCES'
deb [arch=armhf signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://ports.ubuntu.com/ubuntu-ports noble main universe
deb [arch=armhf signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://ports.ubuntu.com/ubuntu-ports noble-updates main universe
deb [arch=armhf signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://ports.ubuntu.com/ubuntu-ports noble-security main universe
SOURCES
OPTS=(-o "Dir::Etc::sourcelist=$BASE/apt/sources.list" -o Dir::Etc::sourceparts=-
 -o "Dir::State::lists=$BASE/apt/lists" -o "Dir::Cache::archives=$BASE/apt/archives"
 -o "Dir::State::status=$BASE/apt/status" -o "Dir::Log=$BASE/apt/log"
 -o "Dir::Cache::pkgcache=$BASE/apt/pkgcache.bin" -o "Dir::Cache::srcpkgcache=$BASE/apt/srcpkgcache.bin"
 -o APT::Architecture=armhf -o APT::Architectures::=armhf
 -o Acquire::Languages=none -o Debug::NoLocking=1)
apt-get "${OPTS[@]}" update
apt-get "${OPTS[@]}" -y --download-only --no-install-recommends install \
 libegl-dev libgles-dev libgl1-mesa-dri
for pkg in "$BASE"/apt/archives/*.deb; do
 dpkg-deb -x "$pkg" "$BASE/root"
done
mkdir -p "$BASE/root"/{dev,proc,sys,tmp,usr/bin}
printf '%s\n' 'ARM Mesa runtime extracted under:' "$BASE/root"

# Ubuntu packages use a merged /usr layout.
[ -e "$BASE/root/lib" ] || ln -s usr/lib "$BASE/root/lib"
