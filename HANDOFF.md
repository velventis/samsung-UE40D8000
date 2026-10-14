# Samsung UE40D8000 firmware on the emulated HTC HD2: handoff

## Goal

Boot the Samsung UE40D8000 (2011, Genoa-P / SDP1004) TV firmware on the
HTC HD2 emulator (`~/clones/hd2-qemu`, `-M htcleo`), as far as the TV UI.
Patching the firmware is allowed.

## Approach

The TV kernel can't run on the HD2: it is built for RAM at `0x60000000`,
and the HD2 has none there. So the port runs the TV's userland on the HD2's
own Linux kernel:

| Layer | What runs |
|---|---|
| Bootloader | cLK 1.4 (`~/clones/hd2-qemu/firmware/lk-user.bin`) |
| Kernel | HD2 2.6.32 kernel, branch `samsung-tv` (worktree `kernel/`) |
| Root fs | The TV's own `rootfs.img` (SquashFS) on HD2 NAND `system` (`mtdblock4`) |
| TV app | Original `exeDSP` + `libSDAL.so` from `exe.img` |
| TV drivers | `shim/libhd2tv.so`, preloaded: emulates `/dev/mem`, `samdrv.ko`'s `/dev/sam/*` and the TV kernel's `/dev/sdp_*` |

The TV's eMMC and OneNAND partitions are SD card partitions. The TV's boot
scripts (`/etc/profile`, `/mtd_exe/rc.local`) run nearly unmodified.

## Layout

```
dl/ extract/ dec/       firmware download, decrypted images (git-ignored)
sq_*/                   unpacked images, for reading only (unreliable: hardlinks lost)
work/                   unsquashfs output and temp files (git-ignored)
out/                    nand.img, sdcard.img, rootfs.img, exe.img (git-ignored)
run/                    emulator run dir: logs, sockets, smem.bin (git-ignored)
bin/qemu-tv             copy of hd2-qemu's qemu-system-arm (git-ignored)
kernel/  kbuild/        HD2 kernel worktree (branch samsung-tv) and its build dir
overlay/rootfs/         files changed or added in the TV rootfs
overlay/exe/            files changed or added in exe.img (rc.local, hd2/libhd2tv.so)
shim/                   libhd2tv.so sources, tools/ (hd2daemon, memdump)
scripts/                build, run and debug scripts
re/                     reverse-engineering output; re/ghidra holds the Ghidra projects
                        (sdal, samdrv, exedsp)
tools/squashfs-tools/   squashfs-tools built from source (git-ignored)
```

## Build and run

```bash
scripts/build-kernel.sh          # HD2 kernel -> kbuild/arch/arm/boot/zImage
scripts/mkimages.py              # out/{rootfs,exe,nand,sdcard}.img
scripts/start.sh                 # boot in the background (cLK menu: F2 pressed)
scripts/stop.sh                  # stop this checkout's emulator only
```

- **`scripts/cycle.sh`**: rebuilds the shim and images, reboots, then waits.
  - Set `ROOTFS=1` to also rebuild the rootfs and NAND.
  - Set `CMDLINE_EXTRA=hd2noexe` to boot without starting `exeDSP`; use this for development.
- **SD rebuilds keep FAT partitions:** the rwarea's settings and EEPROM files survive, and only the SquashFS partitions are rewritten. `SD_FRESH=1` recreates the card.
- **Never use `/tmp`:** clang writes its temp files to `work/tmp`.
- **Emulator ports:**
  - The console is UART1 on `run/console.sock`, logged to `run/uart1.log`.
  - adb is on `127.0.0.1:5575`; port 5555 belongs to the other HD2 checkout.

### A real HD2

`scripts/mkhd2zip.py` packages `out/hd2-samsung-ue40d8000.zip` for an HD2
with cLK 1.5: `boot.img` (without `hd2noexe`, the board's memory map),
`boot-extramem.img` (also RAM at 0x30000000, unverified on hardware),
`system.img`, `sdcard.img`, fastboot scripts for the partition layout
(`misc recovery sboot boot system cache userdata`: the root is mtdblock4) and
a README. Never tried on a real HD2. Build the kernel and all images first.

### Fast dev loop

1. Boot once with the TV app off:

   ```bash
   CMDLINE_EXTRA=hd2noexe ROOTFS=1 scripts/cycle.sh
   ```

2. Rebuild the shim, push it over adb and start `exeDSP` detached:

   ```bash
   scripts/devrun.sh
   ```

   It writes `/hd2/log/exedsp.log`, and `HD2TV_LOG=<mask>` sets the logging.

3. Read the results:
   - `scripts/getlog.sh`: pulls the log to `run/exedsp.{log,txt}`.
   - `scripts/con.py 'cmd'`: runs a command on the TV's UART shell (`VDLinux#>`).
   - `adb -s 127.0.0.1:5575 shell`: root shell through `adbd` (`/sbin/adbd`, from inittab).

### Debug helpers

| Script | What it does |
|---|---|
| `scripts/threads.sh` | `exeDSP` threads: state, wchan, user PC (symbolized) |
| `scripts/stack.sh TID` | Return addresses on a thread's stack (via the static `memdump`, ptrace-attached) |
| `scripts/sym.py ADDR…` | Function for an address in `exeDSP` or the prelinked TV libraries |
| `scripts/decomp.sh PROJ PROG OUT name\|0xaddr\|xref:"str"\|refs:0xaddr` | Ghidra headless decompile (projects `sdal`, `samdrv`, `exedsp`) |
| `scripts/shot.sh [out.png]` | Screenshot of the HD2 screen (QMP screendump) |
| QMP `info registers` loop | Samples the running PC to find spinners: `python3 ~/clones/hd2-qemu/tools/qmon.py run/qmp.sock "info registers"` |

`exeDSP` keeps its dynamic C++ symbols, so `llvm-objdump -d -C` gives
readable names.

## Kernel changes (`kernel/`, branch `samsung-tv`)

- **Config:** SysV IPC, POSIX mqueue, SquashFS, `SERIAL_MSM` with console, paranoid network off.
- **`msm_serial`:**
  - The tty uses major 204, minor 64, so the TV's `ttyS0`–`ttyS3` nodes work.
  - MISR is read as ISR & IMR: the emulator returns CR at offset 0x10, which broke RX interrupts.
- **mmc block:** 16 minors per card, for the TV's `mmcblk0p1`–`p15`.
- **vfat:** also registered as `rfs` (Samsung RFS).
- **`htcleo_nand`:** sub-page reads go through a bounce buffer, so `mtdblock` can serve SquashFS.

## Image patches (`overlay/`, built by `mkimages.py`)

- **rootfs device nodes:**
  - `ttyS2` → `204:64`, the HD2's UART1.
  - `stl0/13` → `mmcblk0p9` (exe).
  - `stl0/12` → `mmcblk0p10` (rwarea).
  - `fb0` added.
- **`/etc/rc.local`** additions:
  - an Android wakelock, because the kernel suspends without one;
  - a `/hd2` tmpfs;
  - a writable `/dev` copy with ptys and the adb gadget nodes.
- **`/etc/inittab`:** `adbd` respawn.
- **`/sbin`:** `adbd` (static, patched to stay root), `hd2daemon`, `memdump`.
- **`/mtd_exe/rc.local`:**
  - `exeDSP` runs with `LD_PRELOAD=/mtd_exe/hd2/libhd2tv.so` and logs to `/hd2/log`.
  - The `hd2noexe` kernel argument skips it.
- **Kernel command line:** starts with `x=/dev/tfsr6` so `/etc/profile` picks the first partition set.

### SD card partitions

| Partition | Size | Contents | TV mount |
|---|---|---|---|
| p1 | 16 MiB | FAT | swu |
| p2 | 46 MiB | `out/rocommon.img` (SquashFS) | rocommon (UI images, from a UE46D5520) |
| p3 | 16 MiB | FAT | emanual |
| p5 | 64 MiB | FAT | contents |
| p6 | 45 MiB | `appext.img` (SquashFS) | appext |
| p7 | 4 MiB | FAT | appext, second copy |
| p8 | 64 MiB | FAT | rwcommon |
| p9 | 81 MiB | `exe.img` (SquashFS) | exe |
| p10 | 32 MiB | FAT | rwarea |

The card is padded to 512 MiB, because QEMU needs a power of two. When the
layout changes, FAT partitions that keep their number and size are copied
over from the old card (`SD_FRESH=1` formats them anew).

### `/mtd_rocommon`: UI images from a UE46D5520RB NAND dump

The TV's ROM partition (`Images_960x540`, fonts) is written at the factory
and is not in the firmware update. `UE46D5520RB NAND.zip` (repo root,
git-ignored) is a raw dump of a sibling model's 2 GiB MLC NAND that has one.

- **Geometry:** blocks of 128 pages; pages of 8192 data + 436 spare bytes
  (the 1,104,384-byte blocks look like 256 pages of 4314 in a hex dump: they aren't).
- **ECC, not applied in the dump** (about one bit flip per 4 KB): BCH over
  GF(2^14), polynomial `0x402b`, t = 24, per 1024-byte sector. The codeword is
  the sector, then its 12 free OOB bytes, then 42 ECC bytes, bits LSB first.
  The spare is 8 × [12 OOB + 42 ECC] + 4. `scripts/nand/nandecc.c` corrects it.
- **Flash layout:** Samsung FSR. `FSRPART` near the end lists 5 STL partitions:
  `0x1b`–`0x1e` are FAT32 (RFS), and `0x1f` (blocks 1724–1923) holds a
  45 MB SquashFS, the D5520's rocommon. STL keeps its 1 MiB logical units in
  physical blocks out of order (pages within a unit are in order), and its
  map wasn't decoded. `scripts/nand/extract_rocommon.py` orders them from the
  filesystem itself: the inode/fragment tables say where each compressed block
  starts, and every unit goes where those blocks decompress.

```bash
unzip "UE46D5520RB NAND.zip" -d work/d5520
scripts/nand/extract_rocommon.py "work/d5520/UE46D5520RB NAND.bin" dec/d5520-rocommon.img
```

`mkimages.py --only rocommon` (run by `cycle.sh` when `out/rocommon.img` is
missing, or with `ROCOMMON=1`) adds the D8000's names the D5520 has as PNG,
or in another folder, converted to 32-bpp BMP: 1960 of the 4446 images
`exeDSP` names exist. Most of `Common`, `Menu`, `MenuAnimation` and
`TVViewer` are not there (on the D5520 they were on its other flash). The
shim stands in for the rest (`images.c`), and for the missing TV fonts with
Liberation Sans (`/mtd_exe/hd2/font.ttf`).

## The shim (`shim/libhd2tv.so`)

It is built with clang against the TV's glibc 2.11 (`-nostdlib`, own
`tvlibc.h`). It drops `LD_PRELOAD` for `exeDSP`'s children.

### `/dev/mem`

Backed by a sparse 4 GiB file on the `/hd2` tmpfs, at offset = physical
address. That covers:
- the DDR banks at `0x6…` and `0x8…`;
- the register blocks at `0x30000000` (12 MiB) and `0x20000000`.

`regs.c` presets register values the firmware polls for; for now only
`0x303f000c = 6`.

### samdrv devices

- **The ioctl convention** (from the decompiled `samdrv.ko`): the arg is
  `{void *param; int ret}`, and the driver writes its status to `ret`. The
  default handler (`sam_ioctl`) reports success and leaves outputs alone.
- **`sys`** (`devices.c`):

  | ioctl | Meaning | What the shim does |
  |---|---|---|
  | `0x5902` / `0x5903` | Register peek/poke | Done through `/dev/mem` |
  | `0x5905` | `SetMemoryMap` | Stores `u32[0x82]`, readable with `hd2tv_memcfg(id)` |
  | `0x5909` | Kernel memory sizes | Returns 388/198/512/512 MiB, as `CheckMemoryMap` expects |

- **`gfx`** (`gfx.c`): the driver's per-plane state (Set ioctls stored,
  Get ioctls return it; argument sizes from `samdrv.ko`'s copy_from/to_user),
  the planes' back/front buffers as `devGfx_InitInfo` lays them out, and the
  GA done by the CPU: `FillRect` `{dst, pitch, fmt, x, y, w, h, color}`,
  `BltCopy` `{fmt, src, spitch, sx, sy, w, h, dst, dpitch, dx, dy}`, `BltROP`
  `{mode, fmt, src, spitch, sx, sy, w, h, dst, dpitch, dx, dy, rule, key,
  alpha, ...}` (rule is `SdROPMode_k`; drawn as source-over except 2 colour
  key, 0xb constant alpha, 0xc clear), `UpdateOsd*` copy back to front.
  Planes: gfx0 1280×720, gfx1 1920×1080, gfx2 960×540 (format `0x10` =
  ARGB8888), gfx3 the GA's scratch/ROP memory (the UI composes there, at
  `0x974d86c0`, pitch `0x1400`, then blits to gfx0).
- **`se`** (the security engine):
  - `devices.c`: `lldSe_SetDMAStartEnable` is wrapped; each DMA start queues a
    completion `{channel, 0}` that the SE ISR thread reads from the device.
    Without it the CI+ manager (`CCipSecureTime::t_ReadFromNVRAM`) waits
    forever on the DMA semaphore.
  - `crypto.c`: the cipher jobs (`uldSe_SetCpDesDerived`: DES, 3DES, AES-128/256
    in ECB/CBC/CTR) are done by the CPU when software gives the key (key mode
    1). Jobs with the chip's own keys (key modes 2-5, efuse) still go to the
    emulated engine, which leaves the data as it is. `HD2TV_LOG` 0x200 logs the
    jobs (keys of decryptions), 0x400 adds exeDSP return addresses.

- **`ae`** (`ae.c`, the audio DSP):
  - Init returns the instance number as the handle.
  - The buffer-info ioctls follow `lldAe_InitMemBase`: memcfg `0x20` plus fixed offsets, with sizes as in `lldAe_GetMemConfig`.
  - `IrqRead` (`0x70`) and `GetPcmDumpEvent` (`0xa7`) block.
- **Reads on samdrv devices** block, since they are interrupt waits. NRSS is the exception: each thread's first read returns zeros, later ones block.

### Other devices and setup

- **I2C** (`i2c_gpio.c`):
  - The ioctl struct is `{u8 slave; u8 subaddr_len; u16 speed; u16 ?; u16 pad; u32 len; u8 *subaddr; u8 *data}`.
  - Ioctl `1` writes, `2` reads, `0x11` reads after a subaddress, and `0x12` does write-then-read.
  - Ports 0–4 are `/dev/sdp_i2cN`, 5–7 are `/dev/sdp_m_i2cN`.
  - EEPROMs (`0xA0`–`0xAE`) persist in `/mtd_rwarea/hd2-eeprom-<port>-<addr>.bin`, zero-filled: 0xff crashed the EEPROM store.
- **GPIO:**
  - Ioctl `0x4a` configures, `0x4b` writes, `0x4c` reads.
  - The arg is `{u16 port<<4|bit; u16 pad; u8 value}`.
  - Reads return the last value written, or 0.
- **UARTs:** `/dev/ttyS1` (MICOM) and `/dev/ttyS3` (debug) get the slave
  side of a pty. On the MICOM's, `micom.c` acks each command (`ff ff cmd
  p0..p4 sum`, echoed back), except the codes the TV parses as MICOM events.
- **GL, on the HD2's Adreno 200** (`gl.c`, `hd2gl/`): exeDSP's UI composes
  with GLES2 (`Shadow::GLRenderer`) into EGL pixmap surfaces (Mali
  `fbdev_pixmap`s over its screen buffers in GA memory), with GA surfaces as
  textures through EGLImages. `gl.c` provides EGL/GLES2 and forwards the calls
  (batched, names allocated locally; `glproto.h`) to `hd2gl`, an Android
  program on the HD2 ROM's Adreno driver, run in a chroot (`/hd2/android`, set
  up by `hd2/android.sh` from rc.local; KGSL's firmware comes through the
  kernel hotplug helper `hd2/hotplug.sh`).
  - The TV's two shader programs are Mali binaries (`CCGfx::m_InitializeShader`);
    `hd2gl` runs GLSL versions: 2D (textured quads, `alphaConstant`) and the 3D
    panel shader without its lights, fog, depth of field and deforms.
  - Surfaces are FBOs; what is drawn is read back into TV memory at sync points
    (glFinish, eglWaitGL, eglMakeCurrent, before each GA op). Textures from TV
    memory reload when the GA writes there or their contents change.
  - Colours stay B, G, R, A in GL's R, G, B, A channels, like the TV's memory.
  - `mkimages.py` copies the ROM files it needs (`HD2_ROM`, default
    `~/clones/hd2-qemu/firmware/rom/system`) into the exe image under
    `hd2/android/system`; `shim/build.sh` builds `hd2gl` with NDK r23.
- **Display** (`fb.c`):
  - A thread composites the planes that are on (`hd2tv_gfx_planes`: gfx2, gfx1, then gfx0 on top) over black to `/dev/fb0`.
  - The image is rotated 90° and scaled to 800×450 on the portrait panel, at 4 fps.
  - `msm_fb` is 32 bpp (the MDP reads bytes R, G, B, X; pages of 480×800×4), but reports a 960-byte `line_length` and RGB565 colour offsets: the shim ignores both.
  - Override the plane with `HD2TV_PLANE=pa,w,h,stride`.
- **Startup housekeeping:**
  - Removes stale SysV IPC objects, which `exeDSP` leaks when killed. Without this, `msgget` returns ENOSPC.
  - Creates `/mtd_rwarea/MAC_CHANGE`, so the fast-boot MAC-changer thread exits.
  - Makes `/mtd_rwarea/cip_services_database` a directory, so CI+ storage is off.
- **Diagnostics:**
  - `crash.c` prints registers, symbolized PC/LR and stack return addresses on SIGSEGV/SIGBUS/SIGILL.
  - `exits.c` logs `kill()` and `exit()` callers.
  - `trace.c` with `trace.inc` logs return codes of chosen `libSDAL` functions; `HD2TV_LOG` bit 32 enables it.
  - `hd2tv.c` logs failed `/dev` opens and reads returning ≤ 0 on real fds.

## Status

### Working

- The TV userland boots on the HD2: the rootfs, the profile scripts and the SquashFS/RFS mounts.
- `exeDSP` gets through `libSDAL` init (`spISys_Init`), with each module's init returning 0: TSD/TSM, DP, FRC, SIF, Gfx, AVD, AFE, SE, JPEG, MFC, HDMI Rx, PVR, MPEG and the audio engine.
- Fast boot runs, including the EEPROM defaults, which take a few minutes on first boot.
- `main()` → `Main()` → `TCMW::Initialize()` runs to the end ("TCMW::Initialize() end" in the log). The CI+ manager's wait on the crypto engine (SE) DMA semaphore has no timeout; the shim's SE completion events (see `se` above) get it past.
- `APP_INITIALIZE()` runs: factory reset, `CViewerApp::t_ProcessNormalBoot`, the
  "No Signal" message, with real images from rocommon.
- **The UI is on the HD2 screen:** GL on the Adreno into the composition
  buffer (`0x975daa40`, 1280×720), `CCGfx::Update` blits it to gfx0 with the
  GA, `fb.c` shows gfx0. The first screen is the "No Signal" message box.
  - Text in it comes out dark where it is probably light: not looked at yet.
- **Remote keys:** `micom.c` sends lines written to the FIFO `/hd2/rckey` as
  the MICOM's IR key packets (`ff ff 10 code 0 0 0 0 sum`): a key code or a
  name (`smarthub`, `menu`, `source`, `up`/`down`/`left`/`right`, `enter`,
  `return`, `exit`, digits, colours...). Codes are the TV's `KEY_*` values
  (`KEY_CONTENTS` 0x79 is Smart Hub):
  `adb -s 127.0.0.1:5575 shell 'echo smarthub > /hd2/rckey'`.
  The HD2's buttons (`htcleo-keypad`, read from `/hd2/keypad`): Windows =
  Smart Hub, volume rocker = volume keys (repeating while held). In the
  emulator: F1 is the Windows button, PageUp/PageDown the volume rocker.
- **Smart Hub** (InfoLink3) starts and shows its loading indicator: it needs
  `/mtd_down` (`/mtd_rwcommon`, SD p8) mounted, which
  `/etc/Scripts/rest_mount.sh` does with the `MTD_*` variables of
  `/etc/profile` (`devrun.sh` exports them), and copies its manager widget
  from `/mtd_rocommon/infolink` (the D5520's).
  - The widget's files (`*.cmk`) are AES-128-CBC with a "common key"
    (`DecryptWithCommonKey`, `CMK_to_membuf`): one of three 32-byte key+IV
    blobs in exeDSP (`0x358ac5c`, from `FUN_0152215c`) decrypted by the SE
    with its efuse key (key mode 3). That key is in the silicon, but the
    result is SamyGO's published .cmk key and IV
    (https://forum.samygo.tv/viewtopic.php?t=4994): `crypto.c` answers that
    job with it (`efuse_answers`). The Smart Hub home screen then loads.
  - `CBrokerProxyEmp::Execute: connect() failed (111)`: Smart Hub's EMP
    broker service is not running; not looked at yet.
  - Secure storage (`/mtd_rwarea/security`, `/mtd_rwcommon/psa`) written while
    the SE did not encrypt has to be deleted once (done on this card).

### Still to do

- `TCTv::Power(3)`, `TCMW::WatchDog()`.
- **Display:** planes from memcfg: gfx0 `0x949c0000`, gfx1 `0x95880000`, gfx2 `0x96b80000`, gfx3 `0x97480000`; cursor `0x94980000`.
- **MICOM:** acks only; no power-on reason. The TV's command 0x18 (sent on
  volume changes) is not acked (it is a MICOM event code), so each one waits
  out the ack timeout.
- **UI speed:** a few frames a second in the emulator, so short animations
  (the volume bar's fade-in) are barely seen. exeDSP ~40% CPU, hd2gl ~5%: the
  time goes to waits, mostly GPU read-backs at sync points (every GA op).
  `HD2TV_LOG` 0x800 makes hd2gl log draws and uniforms.
- **LCD vs PDP:** a GPIO read selects the panel type; the firmware currently picks PDP while the factory data says LCD.
- **Audio output:** the AE initialises but nothing decodes or plays.

## Gotchas

- **adb:**
  - The adb shell can't hold a detached process; use `/sbin/hd2daemon` (setsid + fork).
  - The process also needs the adb session kept open about 2 s.
  - busybox has no `setsid`, `nohup`, `printf`, `sort`, `uniq`, `wc` or `grep -A/-B`.
- **CPU starvation:** `exeDSP` threads can spin at high priority and starve `adbd` and the shell. If adb hangs, sample PCs over QMP, then kill `exeDSP` from `scripts/con.py`.
- **Spinning on fake devices:** whenever an emulated device returns at once where the real driver would block, a thread spins. Check `scripts/threads.sh` for R-state threads with high utime.
- **Background `exeDSP`:** its `stdin` must block. It is a `tail -f /hd2/stdin` pipe; `/dev/null` makes `UartInputBase` spin.
- **Read logs on the host:** pull them with `scripts/getlog.sh`, because grepping large logs over adb is slow.
- **Rebuilding the SD card** with `SD_FRESH=1` loses the EEPROM files, so first boot rewrites the defaults.
- **Don't kill other emulators:** another HD2 emulator (`~/clones/hd2-qemu` or `~/clones/hd2-wm`) may be running. Stop this one with `scripts/stop.sh`, never by process name.
