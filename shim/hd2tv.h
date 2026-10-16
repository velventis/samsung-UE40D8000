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
uint32_t hd2tv_memcfg(unsigned id);
void hd2_regs_init(void);
void hd2_ipc_clean(void);
void hd2_fb_init(void);
void hd2_crypto_selftest(void);
void hd2_micom_start(int master);
void hd2_debug_uart_start(int master);
const char *hd2_image_path(const char *path, char *buf, size_t n);
uint64_t hd2tv_va2phys(const void *va);
int hd2tv_phys_fd(void);
void hd2tv_gl_sync(void);
void hd2tv_gl_dirty(uint32_t pa, uint32_t len);
struct hd2tv_view { uint32_t pa, w, h, stride; };   /* an ARGB8888 plane */
int hd2tv_gfx_planes(struct hd2tv_view *v, int max);
