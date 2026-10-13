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
| p2 | 8 MiB | FAT | rocommon (empty) |
| p3 | 16 MiB | FAT | emanual |
| p5 | 64 MiB | FAT | contents |
| p6 | 45 MiB | `appext.img` (SquashFS) | appext |
| p7 | 4 MiB | FAT | appext, second copy |
| p8 | 64 MiB | FAT | rwcommon |
| p9 | 81 MiB | `exe.img` (SquashFS) | exe |
| p10 | 32 MiB | FAT | rwarea |

The card is padded to 512 MiB, because QEMU needs a power of two.

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

- **`gfx`** (`devices.c`):

  | ioctl | Returns | Memory ids used |
  |---|---|---|
  | `0x4711` | Plane base and size | `0x28`, `0x2a`, `0x2c`, `0x30` |
  | `0x4712` | ROP buffer | `0x7e` |
  | `0x47aa` | Cursor | `0x26` |

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
- **UARTs:** `/dev/ttyS1` (MICOM) and `/dev/ttyS3` (debug) get the slave side of a pty that nobody answers on.
- **Display** (`fb.c`):
  - A thread copies graphics plane 0 (memcfg `0x28`, assumed 960×540 ARGB8888) to `/dev/fb0`.
  - The image is rotated 90° and scaled to 800×450 on the portrait panel, at 4 fps.
  - `msm_fb` reports 32 bpp but is RGB565 (960-byte lines), so the pixel size comes from `line_length`.
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
- `main()` → `Main()` → `TCMW::Initialize()` starts.

### Blocked

- **Where:** `TCMW::Initialize` → `TCTvMiddleware_EU::t_InitMiddlewareModule` → `TCCIM::Instance()->Create()` → `TCCIMManagerPlus::Create`.
- **What happens:** that path hands stored CI+ data to the hardware crypto engine (SE) and waits forever for its DMA-completion interrupt.
- **Why it's unresolved:** the previous agent stopped here. Getting past it means emulating that crypto engine or changing CI+ behaviour, and it declined that work.

### After that, still to do

- The rest of `TCMW::Initialize`, then `APP_INITIALIZE()`, `TCTv::Power(3)`, `TCMW::WatchDog()`.
- **Display:** confirm which plane and format the UI draws into. The `fb.c` default is a guess.
  - Planes from memcfg: gfx0 `0x949c0000`, gfx1 `0x95880000`, gfx2 `0x96b80000`, gfx3 `0x97480000`; cursor `0x94980000`.
  - Planes are drawn by the GA (2D accelerator) through registers. Blits may need emulating, or overriding at the `SdGfx_*` / `spIIoGfx_*` level, which is interposable through the PLT.
- **Mali** (`/dev/sam/mali`): `libMali` asks for the API version first (ioctl `0xc0048206`). Only needed if the UI uses GLES.
- **MICOM / remote input:** not emulated. The silent UART means no power-on reason and no remote keys. Options:
  - drive input through the debug stdin pipe: `echo … >> /hd2/stdin`; `UartInputBase` accepts codes such as `1194444` and `8158282`;
  - or use the HD2's keys and touch screen.
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
