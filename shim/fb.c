/*
 * The TV's picture on the HD2 screen: a thread copies a TV graphics
 * plane (in the emulated TV memory) to the HD2 framebuffer, rotated to
 * the portrait panel and scaled to its height.
 *
 * HD2TV_PLANE=pa,width,height,stride picks the plane (ARGB8888);
 * HD2TV_FPS the refresh rate.
 */
#include "hd2tv.h"

struct fb_var {                 /* struct fb_var_screeninfo */
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    uint32_t red[3], green[3], blue[3], transp[3];
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, reserved[5];
};
struct fb_fix {                 /* struct fb_fix_screeninfo */
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t reserved[3];
};
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606

static uint32_t src_pa = 0, src_w = 960, src_h = 540, src_stride = 0;

static void *fb_thread(void *arg)
{
    struct fb_var v;
    struct fb_fix f;
    int fd, fps = 10, page = 0;
    uint8_t *fb;
    const uint32_t *src;
    const char *e;

    (void)arg;
    sleep(2);
    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) {
        hd2tv_log("fb: no /dev/fb0");
        return 0;
    }
    fb = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        hd2tv_log("fb: mmap failed");
        return 0;
    }
    if ((e = getenv("HD2TV_FPS")))
        fps = atoi(e) > 0 ? atoi(e) : 10;
    while (!src_pa && !(src_pa = hd2tv_memcfg(0x28)))  /* graphics plane 0 */
        sleep(1);
    if (!src_stride)
        src_stride = src_w * 4;
    src = hd2tv_phys(src_pa, src_stride * src_h);
    hd2tv_log("fb: %ux%u %ubpp (virtual %u), plane %08x %ux%u",
              v.xres, v.yres, v.bits_per_pixel, v.yres_virtual, src_pa,
              src_w, src_h);
    if (!src || (v.bits_per_pixel != 16 && v.bits_per_pixel != 32))
        return 0;
    hd2tv_log("fb: rgb offsets %u/%u/%u, line %u", v.red[0], v.green[0],
              v.blue[0], f.line_length);

    /* landscape TV on the portrait panel: rotated 90 degrees clockwise,
     * the TV's width along the panel's height */
    uint32_t ow = v.yres, oh = ow * src_h / src_w;  /* 800 x 450 */
    uint32_t x0 = (v.xres - oh) / 2;
    int pages = v.yres_virtual >= 2 * v.yres ? 2 : 1;

    for (;;) {
        uint8_t *dst = fb + (size_t)page * v.yres * f.line_length;

        for (uint32_t py = 0; py < v.yres; py++) {      /* along TV x */
            uint32_t sx = py * src_w / ow;
            uint8_t *row = dst + py * f.line_length;
            for (uint32_t px = 0; px < v.xres; px++) {
                uint32_t r = 0, g = 0, b = 0;
                if (px >= x0 && px < x0 + oh) {
                    /* panel x grows to the right = TV y from the bottom */
                    uint32_t sy = (oh - 1 - (px - x0)) * src_h / oh;
                    uint32_t c = src[sy * (src_stride / 4) + sx];
                    uint32_t a = c >> 24;
                    r = ((c >> 16) & 0xff) * a / 255;
                    g = ((c >> 8) & 0xff) * a / 255;
                    b = (c & 0xff) * a / 255;
                }
                if (v.bits_per_pixel == 16)
                    ((uint16_t *)row)[px] = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
                else
                    ((uint32_t *)row)[px] = r << v.red[0] | g << v.green[0] |
                                            b << v.blue[0] | 0xffu << v.transp[0];
            }
        }
        v.yoffset = page * v.yres;
        ioctl(fd, FBIOPAN_DISPLAY, &v);
        page = (page + 1) % pages;
        usleep(1000000 / fps);
    }
    return 0;
}

void hd2_fb_init(void)
{
    const char *e = getenv("HD2TV_PLANE");
    pthread_t t;

    if (e) {
        char *p;
        src_pa = strtoul(e, &p, 0);
        if (*p == ',') src_w = strtoul(p + 1, &p, 0);
        if (*p == ',') src_h = strtoul(p + 1, &p, 0);
        if (*p == ',') src_stride = strtoul(p + 1, &p, 0);
    }
    pthread_create(&t, 0, fb_thread, 0);
}
