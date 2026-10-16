/* Samsung OSD planes on a Linux framebuffer: landscape, 1920x1080 canvas.
 * The guest display is configured at 1920x1080. Other modes are letterboxed
 * without rotation, cropping, or changing the TV application's plane sizes.
 */
#include "hd2tv.h"
#include "display_layout.h"

struct fb_var {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    uint32_t red[3], green[3], blue[3], transp[3];
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, reserved[5];
};
struct fb_fix {
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
static struct hd2tv_view fixed;

static bool channel_ok(const uint32_t *c, uint32_t bits)
{
    return !c[2] && c[1] <= 16 && c[0] <= bits && c[1] <= bits - c[0];
}

static void *fb_thread(void *arg)
{
    struct fb_var v;
    struct fb_fix f;
    const char *path = getenv("HD2TV_FB");
    int fd, fps = 15;
    uint8_t *fb;
    (void)arg;
    if (!path) path = "/dev/fb0";
    if (!strcmp(path, "none")) return 0;
    sleep(2);
    fd = open(path, O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) {
        hd2tv_log("fb: cannot query %s", path);
        if (fd >= 0) close(fd);
        return 0;
    }
    uint32_t bytes = v.bits_per_pixel / 8;
    if (!v.xres || !v.yres || (bytes != 2 && bytes != 4) ||
        f.type != 0 || f.visual != 2 || v.nonstd || v.grayscale || v.rotate ||
        !v.red[1] || !v.green[1] || !v.blue[1] ||
        !channel_ok(v.red, v.bits_per_pixel) ||
        !channel_ok(v.green, v.bits_per_pixel) ||
        !channel_ok(v.blue, v.bits_per_pixel) ||
        !channel_ok(v.transp, v.bits_per_pixel) ||
        ((uint64_t)v.xoffset + v.xres) * bytes > f.line_length ||
        ((uint64_t)v.yoffset + v.yres - 1) * f.line_length +
            ((uint64_t)v.xoffset + v.xres) * bytes > f.smem_len) {
        hd2tv_log("fb: unsupported framebuffer geometry/format");
        close(fd);
        return 0;
    }
    fb = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        hd2tv_log("fb: mmap failed");
        close(fd);
        return 0;
    }
    const char *e = getenv("HD2TV_FPS");
    if (e && atoi(e) > 0) fps = atoi(e);
    if (fps > 60) fps = 60;
    struct tv_rect view = tv_fit_16_9(v.xres, v.yres);
    if (!view.w || !view.h) {
        munmap(fb, f.smem_len);
        close(fd);
        return 0;
    }
    hd2tv_log("fb: %ux%u %ubpp stride %u; TV 1920x1080 -> %ux%u at %u,%u; no rotation",
              v.xres, v.yres, v.bits_per_pixel, f.line_length,
              view.w, view.h, view.x, view.y);
    uint32_t *acc = calloc(v.xres, sizeof(*acc));
    if (!acc) {
        munmap(fb, f.smem_len);
        close(fd);
        return 0;
    }
    /* Compute scaling and format conversion outside the pixel loops.
     * ARM32 otherwise performs a 64-bit division for every source pixel. */
    uint32_t pack_r[256], pack_g[256], pack_b[256];
    uint32_t opaque = tv_channel(255, v.transp[0], v.transp[1]);
    for (unsigned i = 0; i < 256; i++) {
        pack_r[i] = tv_channel(i, v.red[0], v.red[1]);
        pack_g[i] = tv_channel(i, v.green[0], v.green[1]);
        pack_b[i] = tv_channel(i, v.blue[0], v.blue[1]);
    }
    uint32_t *xs = malloc(3 * view.w * sizeof(*xs));
    uint32_t widths[3] = {0};
    if (!xs) { free(acc); munmap(fb, f.smem_len); close(fd); return 0; }
    for (;;) {
        struct hd2tv_view pl[3];
        const uint8_t *map[3] = {0};
        int n = fixed.pa ? (pl[0] = fixed, 1) : hd2tv_gfx_planes(pl, 3);
        for (int i = 0; i < n; i++) {
            if (pl[i].w && pl[i].h && pl[i].stride >= pl[i].w * 4)
                map[i] = hd2tv_phys(pl[i].pa, pl[i].stride * pl[i].h);
            if (widths[i] != pl[i].w) {
                for (uint32_t x = 0; x < view.w; x++)
                    xs[i * view.w + x] = (uint64_t)x * pl[i].w / view.w;
                widths[i] = pl[i].w;
            }
        }
        for (uint32_t py = 0; py < v.yres; py++) {
            memset(acc, 0, v.xres * sizeof(*acc));
            if (py >= view.y && py - view.y < view.h)
                for (int i = 0; i < n; i++) {
                    if (!map[i]) continue;
                    uint32_t sy = (uint64_t)(py - view.y) * pl[i].h / view.h;
                    const uint32_t *src = (const uint32_t *)(map[i] + sy * pl[i].stride);
                    for (uint32_t x = 0; x < view.w; x++) {
                        uint32_t sx = xs[i * view.w + x];
                        uint32_t c = src[sx], a = (c >> 24) + (c >> 31);
                        if (!a) continue;
                        if (a == 256) { acc[view.x + x] = c & 0xffffff; continue; }
                        uint32_t d = acc[view.x + x];
                        uint32_t r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * (256 - a)) >> 8;
                        uint32_t g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * (256 - a)) >> 8;
                        uint32_t b = ((c & 255) * a + (d & 255) * (256 - a)) >> 8;
                        acc[view.x + x] = r << 16 | g << 8 | b;
                    }
                }
            uint8_t *row = fb + (py + v.yoffset) * f.line_length + v.xoffset * bytes;
            for (uint32_t x = 0; x < v.xres; x++) {
                uint32_t c = acc[x];
                uint32_t out = pack_r[c >> 16 & 255] |
                    pack_g[c >> 8 & 255] | pack_b[c & 255] | opaque;
                if (bytes == 2) ((uint16_t *)row)[x] = out;
                else ((uint32_t *)row)[x] = out;
            }
        }
        for (int i = 0; i < n; i++)
            if (map[i])
                munmap((void *)((uintptr_t)map[i] & ~0xfffu),
                       pl[i].stride * pl[i].h + ((uintptr_t)map[i] & 0xfff));
        usleep(1000000 / fps);
    }
}

void hd2_fb_init(void)
{
    const char *e = getenv("HD2TV_PLANE");
    pthread_t t;
    if (e) {
        char *p;
        fixed.pa = strtoul(e, &p, 0);
        fixed.w = 1920; fixed.h = 1080;
        if (*p == ',') fixed.w = strtoul(p + 1, &p, 0);
        if (*p == ',') fixed.h = strtoul(p + 1, &p, 0);
        fixed.stride = fixed.w * 4;
        if (*p == ',') fixed.stride = strtoul(p + 1, &p, 0);
    }
    pthread_create(&t, 0, fb_thread, 0);
}
