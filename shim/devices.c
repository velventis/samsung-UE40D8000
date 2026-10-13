/* samdrv.ko's /dev/sam devices and the TV kernel's /dev/sdp_* drivers */
#include "hd2tv.h"

/* samdrv ioctls pass { void *param; int ret }: the driver copies its
 * result to ret */
struct sam_arg { void *buf; int32_t ret; };

int hd2_ae_ioctl(int unit, unsigned long req, void *arg);

/* the default: success, outputs left as they are */
static int sam_ioctl(int unit, unsigned long req, void *arg)
{
    if (arg)
        ((struct sam_arg *)arg)->ret = 0;
    return 0;
}

/* ---------------------------------------------------------------- sys */
/* libSDAL's physical memory map (ids as in AllocPhyMem: base, size) */
static uint32_t memcfg[0x82];

uint32_t hd2tv_memcfg(unsigned id)
{
    return id < 0x82 ? memcfg[id] : 0;
}

struct sys_reg { uint32_t addr, val_or_ptr, size; };

static int sys_ioctl(int unit, unsigned long req, void *arg)
{
    struct sam_arg *a = arg;

    switch (req) {
    case 0x5902: {              /* read a register: { addr, *out, size } */
        struct sys_reg *r = a->buf;
        uint32_t v = 0;
        void *p = hd2tv_phys(r->addr, 4);
        if (p) {
            if (r->size == 1) v = *(volatile uint8_t *)p;
            else if (r->size == 2) v = *(volatile uint16_t *)p;
            else v = *(volatile uint32_t *)p;
            munmap((void *)((uintptr_t)p & ~0xfffu), 0x1000 + ((uintptr_t)p & 0xfff));
        }
        memcpy((void *)(uintptr_t)r->val_or_ptr, &v, r->size > 4 ? 4 : r->size);
        break;
    }
    case 0x5903: {              /* write a register: { addr, value, size } */
        struct sys_reg *r = a->buf;
        void *p = hd2tv_phys(r->addr, 4);
        if (p) {
            if (r->size == 1) *(volatile uint8_t *)p = r->val_or_ptr;
            else if (r->size == 2) *(volatile uint16_t *)p = r->val_or_ptr;
            else *(volatile uint32_t *)p = r->val_or_ptr;
            munmap((void *)((uintptr_t)p & ~0xfffu), 0x1000 + ((uintptr_t)p & 0xfff));
        }
        break;
    }
    case 0x5905:                /* SetMemoryMap */
        memcpy(memcfg, a->buf, sizeof(memcfg));
        for (int i = 0; i < 0x82; i += 2)
            if (memcfg[i + 1])
                hd2tv_log("memcfg %02x: %08x + %08x", i, memcfg[i], memcfg[i + 1]);
        break;
    case 0x5909: {              /* GetKernelMem */
        /* the TV kernel's memory map (Genoa-P DV board): kernel A/B are
         * the Linux parts of the two DDR banks, system A/B the banks */
        uint32_t *m = a->buf;
        m[0] = 388 << 20;
        m[1] = 198 << 20;
        m[2] = 512 << 20;
        m[3] = 512 << 20;
        break;
    }
    }
    return sam_ioctl(unit, req, arg);
}

/* ---------------------------------------------------------------- gfx */
static int gfx_ioctl(int unit, unsigned long req, void *arg)
{
    struct sam_arg *a = arg;
    uint32_t *out = a->buf;
    static const uint8_t plane_id[4] = { 0x28, 0x2a, 0x2c, 0x30 };

    switch (req) {
    case 0x4711:                /* plane base, size */
        out[0] = hd2tv_memcfg(plane_id[unit & 3]);
        out[1] = hd2tv_memcfg(plane_id[unit & 3] + 1);
        break;
    case 0x4712:                /* ROP buffer (plane 3) */
        out[0] = (unit & 3) == 3 ? hd2tv_memcfg(0x7e) : 0;
        out[1] = (unit & 3) == 3 ? hd2tv_memcfg(0x7f) : 0;
        break;
    case 0x47aa:                /* cursor base, size */
        out[0] = hd2tv_memcfg(0x26);
        out[1] = hd2tv_memcfg(0x27);
        break;
    }
    return sam_ioctl(unit, req, arg);
}

/* NRSS: a status read at init, then its event thread waits for
 * interrupts (none come): each thread's first read returns zeros */
static ssize_t zero_read(int unit, void *buf, size_t n)
{
    static long readers[16];
    long tid = syscall(224);
    int i;

    for (i = 0; i < 16 && readers[i] && readers[i] != tid; i++)
        ;
    if (i < 16 && readers[i] == tid)
        for (;;)
            sleep(3600);
    if (i < 16)
        readers[i] = tid;
    memset(buf, 0, n);
    return n;
}

static const struct hd2_dev devs[] = {
    { "sys", 0, 0, sys_ioctl },
    { "ae", 0, 0, hd2_ae_ioctl }, { "aio", 0, 0, sam_ioctl }, { "avd", 0, 0, sam_ioctl }, { "dbg", 0, 0, sam_ioctl }, { "dp", 0, 0, sam_ioctl }, { "gfx", 0, 0, gfx_ioctl },
    { "mfc", 0, 0, sam_ioctl }, { "mpeg", 0, 0, sam_ioctl }, { "nrss", 0, 0, sam_ioctl, 0, zero_read }, { "playback", 0, 0, sam_ioctl }, { "record", 0, 0, sam_ioctl },
    { "se", 0, 0, sam_ioctl }, { "shdsp", 0, 0, sam_ioctl }, { "tsd", 0, 0, sam_ioctl }, { "tsdqueue", 0, 0, sam_ioctl }, { "Ci", 0, 0, sam_ioctl }, { "Jpeg", 0, 0, sam_ioctl },
    { "Mspi" }, { "h264", 0, 0, sam_ioctl }, { "mali" },
    { "sdp_hwmem", "/dev/sdp_hwmem" },
};
static const struct hd2_dev *ext[] = {
    &hd2_dev_i2c, &hd2_dev_m_i2c, &hd2_dev_gpio,
};

const struct hd2_dev *hd2_dev_find(const char *path, int *unit)
{
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        const struct hd2_dev *d = &devs[i];
        if (d->path) {
            if (!strcmp(path, d->path)) {
                *unit = 0;
                return d;
            }
            continue;
        }
        if (strncmp(path, "/dev/sam/", 9))
            continue;
        size_t n = strlen(d->name);
        const char *u = path + 9 + n;
        if (!strncmp(path + 9, d->name, n) && (*u == 0 || (*u >= '0' && *u <= '9'))) {
            /* "mpeg" must not match "mpeg2..." names of other devices */
            *unit = *u ? atoi(u) : 0;
            return d;
        }
    }
    /* I2C ports 0-4: /dev/sdp_i2cN, 5-7: /dev/sdp_m_i2cN */
    if (!strncmp(path, "/dev/sdp_i2c", 12) && path[12] >= '0' && path[12] <= '9') {
        *unit = atoi(path + 12);
        return &hd2_dev_i2c;
    }
    if (!strncmp(path, "/dev/sdp_m_i2c", 14) && path[14] >= '0' && path[14] <= '9') {
        *unit = 5 + atoi(path + 14);
        return &hd2_dev_m_i2c;
    }
    for (unsigned i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (!strcmp(path, ext[i]->path)) {
            *unit = 0;
            return ext[i];
        }
    return 0;
}

void hd2_devices_init(void)
{
}
