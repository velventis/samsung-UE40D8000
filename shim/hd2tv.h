#pragma once
#include "tvlibc.h"

#define HD2TV_PHYS_FILE "/hd2/phys"

extern unsigned hd2tv_log_mask;
void hd2tv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* the TV physical address range [pa, pa + len) */
void *hd2tv_phys(uint32_t pa, uint32_t len);

/* an emulated device node: /dev/sam/<name><unit> or a full path */
struct hd2_dev {
    const char *name;
    const char *path;           /* NULL: /dev/sam/<name>N */
    void (*open)(int unit);
    int (*ioctl)(int unit, unsigned long req, void *arg);
    /* physical address for an mmap at offset off (default: off) */
    uint64_t (*mmap)(int unit, uint64_t off, uint32_t len);
    ssize_t (*read)(int unit, void *buf, size_t n);
    ssize_t (*write)(int unit, const void *buf, size_t n);
};

const struct hd2_dev *hd2_dev_find(const char *path, int *unit);
void hd2_devices_init(void);

extern const struct hd2_dev hd2_dev_i2c, hd2_dev_m_i2c, hd2_dev_gpio;
void hd2tv_gpio_set_input(int port, int bit, int val);
void hd2tv_crash_init(void);
