/* samdrv.ko's /dev/sam devices and the TV kernel's /dev/sdp_* drivers */
#include "hd2tv.h"

/* samdrv ioctls pass { void *buf; ... } */
struct sam_arg { void *buf; uint32_t size; };

/* ---------------------------------------------------------------- sys */
/* the TV kernel's memory map (Genoa-P DV board): kernel A/B are the
 * Linux parts of the two DDR banks, system A/B the banks */
static int sys_ioctl(int unit, unsigned long req, void *arg)
{
    struct sam_arg *a = arg;

    switch (req) {
    case 0x5909: {              /* GetKernelMem */
        uint32_t *m = a->buf;
        m[0] = 388 << 20;
        m[1] = 198 << 20;
        m[2] = 512 << 20;
        m[3] = 512 << 20;
        return 0;
    }
    }
    return 0;
}

/* NRSS (the CI+ crypto block): status reads at init */
static ssize_t zero_read(int unit, void *buf, size_t n)
{
    memset(buf, 0, n);
    return n;
}

static const struct hd2_dev devs[] = {
    { "sys", 0, 0, sys_ioctl },
    { "ae" }, { "aio" }, { "avd" }, { "dbg" }, { "dp" }, { "gfx" },
    { "mfc" }, { "mpeg" }, { "nrss", 0, 0, 0, 0, zero_read }, { "playback" }, { "record" },
    { "se" }, { "shdsp" }, { "tsd" }, { "tsdqueue" }, { "Ci" }, { "Jpeg" },
    { "Mspi" }, { "h264" }, { "mali" },
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
