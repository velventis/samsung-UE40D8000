/* samdrv's AE (audio engine, the audio DSP) device: init and buffers */
#include "hd2tv.h"

struct sam_arg { void *buf; int32_t ret; };

/* lldAe_InitMemBase: the AE's buffers in its region (memcfg 0x20) */
static const uint32_t ae_off[] = {      /* even AE memory ids 0..0x30 */
    0xb80000, 0xbc0000, 0xc00000, 0xc08000, 0xc10000, 0xc30000, 0xc3c000,
    0xc60000, 0xc64000, 0xc70000, 0xc7c000, 0xc9c000, 0xca0000, 0xca4000,
    0xce0000, 0xcf4000, 0xd34000, 0xd3c000, 0xd78000, 0xda2000, 0xdcc000,
    0xdf6000, 0xe20000, 0xe4a000, 0xe74000,
};

/* lldAe_GetMemConfig */
static uint32_t ae_mem(unsigned id)
{
    uint32_t base = hd2tv_memcfg(0x20);

    if (!(id & 1) && id <= 0x30)
        return base + ae_off[id / 2];
    switch (id) {
    case 0x32: return base + 0xe7c000;
    case 0x35: return base + 0xe84000;
    case 0x37: return base + 0xe84800;
    case 0x36: case 0x38: return 0x800;
    case 0x39: return base + 0xca0000;
    case 0x3a: return 0xd8000;
    case 0x3e: return 0x4000;
    case 0x0f: case 0x17: case 0x19: return 0x4000;
    case 0x05: case 0x07: case 0x21: case 0x31: case 0x33: return 0x8000;
    case 0x0b: case 0x11: case 0x13: return 0xc000;
    case 0x1d: return 0x14000;
    case 0x09: case 0x15: return 0x20000;
    case 0x0d: return 0x24000;
    case 0x1b: case 0x23: return 0x3c000;
    case 0x25: case 0x27: case 0x29: case 0x2b: case 0x2d: case 0x2f: return 0x2a000;
    case 0x01: case 0x03: case 0x1f: return 0x40000;
    }
    hd2tv_log("ae: memory id 0x%x?", id);
    return 0;
}

/* { handle, *addr, *size } */
struct ae_buf { uint32_t handle; uint32_t *addr, *size; };

static void buf(struct ae_buf *b, int id)
{
    *b->addr = id < 0 ? 0 : ae_mem(id);
    *b->size = id < 0 ? 0 : ae_mem(id + 1);
}

int hd2_ae_ioctl(int unit, unsigned long req, void *arg)
{
    struct sam_arg *a = arg;
    uint32_t *p = a->buf;
    struct ae_buf *b = a->buf;
#define idx (p[0])

    switch (req) {
    case 0x70:                  /* IrqRead: waits for the audio DSP */
    case 0xa7:                  /* GetPcmDumpEvent: likewise */
        for (;;)
            sleep(3600);
    case 0x68:                  /* Init { index, config, *handle } */
        *(uint32_t *)(uintptr_t)p[2] = idx;     /* the handle: the index */
        break;
    case 0x90:                  /* GetFileBufInfo */
        buf(b, idx == 0 ? 0 : idx == 1 ? 2 : 0x1a);
        break;
    case 0x93:                  /* GetClipMixBufInfo */
        buf(b, 0x1e);
        break;
    case 0xa3:                  /* GetPcmDumpBufInfo */
        buf(b, 0x28);
        break;
    case 0xb3:                  /* GetDecMainUniBufInfo */
        buf(b, idx == 0 ? 10 : idx == 1 ? 0x12 : -1);
        break;
    case 0xbb:                  /* GetESBufInfo */
        buf(b, idx == 0 ? 0x10 : -1);
        break;
    case 0xd4:                  /* GetESInBufInfo */
        buf(b, idx == 2 ? 0x1c : -1);
        break;
    case 0xdb:                  /* GetFileBufInfoMixing */
        buf(b, idx == 2 ? 0x1e : -1);
        break;
    case 0xe4:                  /* GetMemConfig { id, *value } */
        *(uint32_t *)(uintptr_t)p[1] = ae_mem(idx);
        break;
    }
#undef idx
    if (a)
        a->ret = 0;
    return 0;
}
