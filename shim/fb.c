/*
 * The TV's picture on the HD2 screen: a thread composites the graphics
 * planes that are on (in the emulated TV memory, see gfx.c) over black
 * and copies them to the HD2 framebuffer, rotated to the portrait panel
 * and scaled to its height.
 *
 * HD2TV_PLANE=pa,width,height,stride shows that plane (ARGB8888) instead;
 * HD2TV_FPS sets the refresh rate.
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

static struct hd2tv_view fixed;          /* HD2TV_PLANE */

#define TV_W 1920
#define TV_H 1080

static void *fb_thread(void *arg)
{
    struct fb_var v;
    struct fb_fix f;
    int fd, fps = 4, page = 0;
    uint8_t *fb;
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
        fps = atoi(e) > 0 ? atoi(e) : 4;
    hd2tv_log("fb: %ux%u %ubpp (virtual %u)", v.xres, v.yres, v.bits_per_pixel,
              v.yres_virtual);
    /* msm_fb reports 32 bpp for its RGB565 framebuffer: trust the line */
    v.bits_per_pixel = f.line_length * 8 / v.xres;
    if (v.bits_per_pixel != 16 && v.bits_per_pixel != 32)
        return 0;
    hd2tv_log("fb: rgb offsets %u/%u/%u, line %u", v.red[0], v.green[0],
              v.blue[0], f.line_length);

    /* landscape TV on the portrait panel: rotated 90 degrees clockwise,
     * the TV's width along the panel's height */
    uint32_t ow = v.yres, oh = ow * TV_H / TV_W;    /* 800 x 450 */
    uint32_t x0 = (v.xres - oh) / 2;
    int pages = v.yres_virtual >= 2 * v.yres ? 2 : 1;
    /* TV coordinates (in 1920 x 1080) of each panel row and column */
    uint32_t *tx = calloc(v.yres, 4), *ty = calloc(v.xres, 4);
    uint32_t *acc = calloc(v.xres, 4);

    for (uint32_t py = 0; py < v.yres; py++)
        tx[py] = py * TV_W / ow;
    for (uint32_t px = 0; px < v.xres; px++)
        ty[px] = px >= x0 && px < x0 + oh ? (oh - 1 - (px - x0)) * TV_H / oh : ~0u;

    for (;;) {
        struct hd2tv_view pl[3];
        const uint8_t *map[3];
        int n = fixed.pa ? (pl[0] = fixed, 1) : hd2tv_gfx_planes(pl, 3);
        uint8_t *dst = fb + (size_t)page * v.yres * f.line_length;

        for (int i = 0; i < n; i++)
            map[i] = hd2tv_phys(pl[i].pa, pl[i].stride * pl[i].h);
        for (uint32_t py = 0; py < v.yres; py++) {      /* along TV x */
            uint8_t *row = dst + py * f.line_length;

            memset(acc, 0, v.xres * 4);
            for (int i = 0; i < n; i++) {
                if (!map[i])
                    continue;
                const uint32_t *col = (const uint32_t *)map[i] + tx[py] * pl[i].w / TV_W;
                for (uint32_t px = 0; px < v.xres; px++) {
                    if (ty[px] == ~0u)
                        continue;
                    uint32_t c = col[ty[px] * pl[i].h / TV_H * (pl[i].stride / 4)];
                    uint32_t a = (c >> 24) + (c >> 31), d = acc[px];  /* 0..256 */
                    if (!a)
                        continue;
                    /* over the planes below (the bottom one over black) */
                    uint32_t r = (((c >> 16) & 0xff) * a + ((d >> 16) & 0xff) * (256 - a)) >> 8;
                    uint32_t g = (((c >> 8) & 0xff) * a + ((d >> 8) & 0xff) * (256 - a)) >> 8;
                    uint32_t b = ((c & 0xff) * a + (d & 0xff) * (256 - a)) >> 8;
                    acc[px] = r << 16 | g << 8 | b;
                }
            }
            for (uint32_t px = 0; px < v.xres; px++) {
                uint32_t c = acc[px], r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
                if (v.bits_per_pixel == 16)
                    ((uint16_t *)row)[px] = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
                else
                    ((uint32_t *)row)[px] = r << v.red[0] | g << v.green[0] |
                                            b << v.blue[0] | 0xffu << v.transp[0];
            }
        }
        for (int i = 0; i < n; i++)
            if (map[i])
                munmap((void *)((uintptr_t)map[i] & ~0xfffu),
                       pl[i].stride * pl[i].h + ((uintptr_t)map[i] & 0xfff));
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
        fixed.pa = strtoul(e, &p, 0);
        fixed.w = 960;
        fixed.h = 540;
        if (*p == ',') fixed.w = strtoul(p + 1, &p, 0);
        if (*p == ',') fixed.h = strtoul(p + 1, &p, 0);
        fixed.stride = fixed.w * 4;
        if (*p == ',') fixed.stride = strtoul(p + 1, &p, 0);
    }
    pthread_create(&t, 0, fb_thread, 0);
}
