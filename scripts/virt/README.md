# TV graphics on generic QEMU ARM Linux

The port uses a separate ARM Linux/Mesa EGL/GLES2 renderer and virtio GPU scanout.
It does not launch Android, use the Adreno/KGSL driver, or boot the HD2 kernel.
Mesa softpipe is the tested default. The TV executable keeps its original glibc
2.11 ABI; the renderer has an isolated modern ARMhf runtime.

## Build

In WSL, from ~/samsung-UE40D8000:

```sh
# First run only: download signed ARM packages into work/, without host installs.
bash scripts/virt/prepare-mesa-runtime.sh
# Build Mesa with lazy sampler-cache storage (required for full menus on ARM32).
bash scripts/virt/build-softpipe.sh
bash shim/build-linux.sh
```

The cross compiler arm-linux-gnueabihf-gcc, Clang and LLD are required. The build
outputs are work/virt-mesa/bin/libhd2tv.so and work/virt-mesa/bin/tvgl. The existing
HD2 binaries and build.sh are not overwritten. Downloaded package versions are
recorded in work/virt-mesa/packages.txt after this implementation's build.

## Run

```sh
bash scripts/virt/start-mesa.sh
```

The default opens an SDL window. A lighter 720p output can be selected with:

```sh
TV_SIZE=1280x720 bash scripts/virt/start-mesa.sh
```

Use another WSL terminal for guest setup and startup:

```sh
python3 scripts/virt/console.py 'mount -t squashfs -o ro /dev/vdb /mnt; sh /mnt/prepare.sh' --timeout 45
python3 scripts/virt/console.py 'sh /mnt/run-exedsp.sh'
```

The setup first runs a Mesa self-test covering both replacement shader pairs,
an actual textured draw, and pixel readback. The firmware starts afterward.
It may take a minute or more to finish initialization, particularly on first boot.

```sh
python3 scripts/virt/console.py 'grep -aE "TCMW|APP_INITIALIZE|softpipe|gl:|fb:" /hd2/log/exedsp.log | tail -n 25'
python3 scripts/virt/capture.py
python3 scripts/virt/console.py 'echo menu > /hd2/rckey'
python3 scripts/virt/qmp.py quit
```

The serial socket, QMP socket, console log and screenshots are under work/virt-mesa.
Snapshots protect out/sdcard.img: EEPROM/settings changes from this test are lost
when the VM exits. The helper image mounts read-only and contains the Mesa runtime
and newly built shim; the original firmware images need no rebuild. Guest setup is
intended once per fresh VM. Stop the current VM before rebuilding its helper image.

TV_PROJECT, TV_KERNEL, TV_QEMU and TV_DISPLAY override the project path, kernel,
emulator and display backend. TV_DISPLAY=none runs headless; TV_RAM defaults to
2048 MiB. TV_SIZE accepts 1920x1080 (default) or 1280x720. The tested emulator is the supplied qemu-tv binary
using its standard virt target. TV_QEMU can point to an upstream qemu-system-arm;
this launcher uses generic virt, PCI/virtio block, PL011 and virtio-gpu devices.

## Resolution and orientation

The default output is 1920x1080, landscape, 16:9. TV_SIZE=1280x720 changes
both the virtual GPU mode and kernel video argument. The TV's 1280x720 UI plane
does not gain detail from 1080p output; 720p preserves its aspect ratio and has
56% fewer output pixels to composite. Other TV planes are scaled into the same
16:9 output. Rotation remains disabled in both modes. Other aspect ratios use
letterboxing. Shader coordinates and application surface sizes are unchanged.

The compositor caches horizontal source coordinates and channel conversion
tables, removing the former per-pixel ARM32 64-bit division. Fully transparent
and opaque pixels also skip unnecessary blending. This reduces presentation
work, but ARM TCG emulation plus software GLES still limits overall speed.

The GL-to-TV top/bottom memory conversion remains necessary and is not a display
rotation. BGRA memory conventions and CPU/GL synchronization are also preserved.

## Backend selection and limitations

Softpipe rendered the real TV no-signal screen successfully in the Linux 6.12.113
guest. llvmpipe passed isolated shader and pixel tests but crashed on the complete
TV frame's readback path in this ARM32 setup. Its root cause is not established;
it remains optional through HD2TV_GL_DRIVER=llvmpipe rather than being the default.
HD2TV_GL_SOFTWARE=0 permits hardware selection, but virgl acceleration has not been
validated and requires an appropriate QEMU GL backend and guest Mesa device setup.
Use HD2TV_GL_DRIVER=virgl explicitly for that separate experiment.

Software rendering under ARM emulation is slow, particularly full-frame readbacks.
Some source-project limitations remain: approximate 3D shaders, MICOM timeouts,
incomplete multimedia engines and missing or substituted assets. This port does
not establish full Smart Hub or media playback functionality.

The shim's HD2TV_GL_ROOT chooses the renderer runtime root, HD2TV_GL_RENDERER its
executable inside that root, HD2TV_GL_DRIVER the Mesa driver, HD2TV_FPS the display
refresh cap, and HD2TV_FB=none disables presentation for debugging. The guest
startup script accepts these environment overrides and defaults to softpipe.

## Validation

```sh
cc -O2 -Wall -Wextra -I shim scripts/virt/test-display.c -o work/virt-mesa/test-display
work/virt-mesa/test-display
python3 scripts/virt/test-gl-transport.py
TV_TEST_WIDTH=1280 TV_TEST_HEIGHT=720 python3 scripts/virt/test-gl-transport.py
```

The tests cover native 1080p and 16:10/4:3/ultrawide/portrait letterboxing, RGB565
and XRGB8888 channel packing, inherited descriptors, unaligned physical offsets
above 2 GiB, framebuffer clears, scissoring and upright row readback. The runtime
helper's self-test additionally compiles/links both known shader pairs and draws
through the real 2D shader. Full-system testing covers the original exeDSP,
TCMW::Initialize completion, APP_INITIALIZE completion, and visible 1080p output.

## Files changed

- shim/gl.c: launch an isolated Linux/Mesa process with configurable backend.
- shim/hd2gl/hd2gl.c: Linux large-file mappings, GLES selection, self-test and diagnostics.
- shim/fb.c and shim/display_layout.h: landscape composition and real framebuffer metadata.
- shim/hd2tv.c: unaligned physical mappings and isolated firmware debug UARTs.
- shim/micom.c and shim/hd2tv.h: drain debug UART output and mark injected keys as fresh presses.
- shim/glproto.h: update the protocol description; the wire format is unchanged.
- shim/build-linux.sh and scripts/virt/: independent build, boot, setup and validation.

Original source snapshots are under work/virt-mesa/backups. Existing unrelated
local edits were preserved. No kernel source or configuration changes were needed
for this graphics port after enabling the options in the user's rebuilt kernel.


## Memory and remote input

Stock Ubuntu Mesa 25.2.8 softpipe eagerly allocates roughly 192 MiB of texture
cache storage per context, including unused shader stages and slots. A 2 GiB
guest avoids the original guest OOM, but opening menus still exhausts ARM32
virtual memory. build-softpipe.sh builds the same upstream Mesa version with
mesa-lazy-texture-cache.patch, allocating tile storage only on first texture
lookup. The original isolated runtime is saved under
work/virt-mesa/mesa-build/runtime-before-custom. No host packages are installed.
The source archive hash, pinned Python build tools, cross configuration and
minimal software-driver build are recorded in the build script.

The GL protocol now permits 63 TV contexts plus the helper root (formerly 15).
The 32-context full-size draw/readback and 16-cycle destruction test passed
with roughly 120 MiB RSS, avoiding the former multi-gigabyte address-space use.
Memory diagnostics are emitted every ten seconds of renderer activity.

FIFO keys were reaching KeyInputBase, but its repeat gate rejected Menu with
"t_OkayToSend, false!! #4". Each FIFO line now marks a distinct press using the
firmware's exported KeyCommon::bKeyStopped one-shot flag before delivering the
normal MICOM RC packet. Symbol lookup uses dlsym, with no hardcoded firmware
address. The normal firmware readiness/input gates still run. Each line is a
press; do not repeatedly resend keys merely because animation is slow.

console.py also recognizes a completion marker as the first output line, which
fixes false timeouts after commands with no output. The helper adds a blank
line before the marker so prompts cannot run into it. Firmware debug UARTs
remain separate from the guest control shell.

Additional checks:

```sh
python3 scripts/virt/test-console.py
TV_TEST_CONTEXTS=32 TV_TEST_WIDTH=1280 TV_TEST_HEIGHT=720 TV_TEST_CYCLES=16 \
 python3 scripts/virt/test-gl-transport.py
```

Full-system follow-up: a single Menu key from the no-signal screen opened the
complete settings menu at 1280x720, including labels and controls. The first
opening took about 80 seconds in the tested single-vCPU ARM TCG/softpipe setup.
Thus a long black transition can still occur even after input and allocation
fixes; a successful console marker only confirms the shell command completed.
The supplied qemu-tv binary does not advertise virtio-gpu-gl-pci. GPU offload
requires a separately configured emulator/backend and has not been validated.
