/*
 * The UI's bitmaps: /mtd_rocommon (the factory-written ROM partition with
 * Images_960x540) is not in the firmware update.  Lookups of images there
 * that do not exist go to stand-ins under /hd2/img, written on first use:
 * 32 bpp BMPs, plain translucent boxes with a border, sized from a "_W_H"
 * in the name when there is one.  Fonts (also on /mtd_rocommon) that do
 * not exist are a free font shipped with the shim (/mtd_exe/hd2/font.ttf).
 */
#include "hd2tv.h"

#define IMG_DIR "/hd2/img"

static const char *const font_prefixes[] = {
    "/mtd_appdata/Font/", "/mtd_rocommon/Font/", "/mtd_exe/Font/",
};
static const char *const fonts[] = { "/hd2/font.ttf", "/mtd_exe/hd2/font.ttf" };

static const char *const prefixes[] = {
    "/mtd_appdata/Images_960x540/", "/mtd_rocommon/Images_960x540/",
    "/mtd_exe/Images_960x540/",
};

int __xstat64(int ver, const char *path, void *st);

static bool exists(const char *path)
{
    static int (*real_xstat64)(int, const char *, void *);
    char st[128];

    if (!real_xstat64)
        real_xstat64 = dlsym(RTLD_NEXT, "__xstat64");
    return real_xstat64(3, path, st) == 0;
}

/* "name_332_52.bmp" -> 332 x 52 */
static void size_from_name(const char *name, uint32_t *w, uint32_t *h)
{
    const char *dot = strrchr(name, '.');
    const char *p = dot;
    uint32_t a = 0, b = 0;

    *w = *h = 32;
    if (!p)
        return;
    while (p > name && p[-1] >= '0' && p[-1] <= '9')
        p--;
    if (p == dot || p == name || (p[-1] != '_' && p[-1] != 'x'))
        return;
    b = atoi(p);
    p--;
    const char *q = p;
    while (q > name && q[-1] >= '0' && q[-1] <= '9')
        q--;
    if (q == p)
        return;
    a = atoi(q);
    if (a > 0 && a <= 1920 && b > 0 && b <= 1080) {
        *w = a;
        *h = b;
    }
}

static void mkdirs(char *path)
{
    for (char *s = path + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(path, 0755);
            *s = '/';
        }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static bool write_bmp(const char *path, uint32_t w, uint32_t h)
{
    static int (*ropen)(const char *, int, ...);
    uint32_t size = w * h * 4;
    uint8_t hdr[54] = { 'B', 'M' };
    int fd;

    if (!ropen)
        ropen = dlsym(RTLD_NEXT, "open");
    fd = ropen(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    put32(hdr + 2, 54 + size);
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, w);
    put32(hdr + 22, h);
    hdr[26] = 1;
    hdr[28] = 32;
    put32(hdr + 34, size);
    syscall(4, fd, hdr, 54);
    uint32_t *row = malloc(w * 4);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            bool edge = x == 0 || y == 0 || x == w - 1 || y == h - 1;
            row[x] = edge ? 0xff8a96a8 : 0xd0283040;      /* BGRA */
        }
        syscall(4, fd, row, w * 4);
    }
    free(row);
    close(fd);
    return true;
}

/* the path to use for path: a stand-in, or path itself */
const char *hd2_image_path(const char *path, char *buf, size_t n)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    const char *rel = 0, *dot;

    if (!path || path[0] != '/' || path[1] != 'm')
        return path;
    for (unsigned i = 0; i < sizeof(font_prefixes) / sizeof(font_prefixes[0]); i++)
        if (!strncmp(path, font_prefixes[i], strlen(font_prefixes[i]))) {
            dot = strrchr(path, '.');
            if (!dot || (strcmp(dot, ".ttf") && strcmp(dot, ".TTF")) || exists(path))
                return path;
            for (unsigned j = 0; j < sizeof(fonts) / sizeof(fonts[0]); j++)
                if (exists(fonts[j])) {
                    if (hd2tv_log_mask & 1)
                        hd2tv_log("font %s -> %s", path, fonts[j]);
                    return fonts[j];
                }
            return path;
        }
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strncmp(path, prefixes[i], strlen(prefixes[i]))) {
            rel = path + strlen(prefixes[i]);
            break;
        }
    dot = rel ? strrchr(rel, '.') : 0;
    if (!dot || strcmp(dot, ".bmp") || exists(path))
        return path;
    snprintf(buf, n, IMG_DIR "/%s", rel);
    pthread_mutex_lock(&lock);
    if (!exists(buf)) {
        uint32_t w, h;
        size_from_name(rel, &w, &h);
        mkdirs(buf);
        if (write_bmp(buf, w, h) && (hd2tv_log_mask & 1))
            hd2tv_log("image %s: %ux%u stand-in", rel, w, h);
    }
    pthread_mutex_unlock(&lock);
    return buf;
}

/* ---- lookups by path (open/open64 are in hd2tv.c) */

#define IMG(p) char img_buf[256]; p = hd2_image_path(p, img_buf, sizeof(img_buf))

FILE *fopen(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    if (!real)
        real = dlsym(RTLD_NEXT, "fopen");
    IMG(path);
    return real(path, mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    if (!real)
        real = dlsym(RTLD_NEXT, "fopen64");
    IMG(path);
    return real(path, mode);
}

int __xstat(int ver, const char *path, void *st)
{
    static int (*real)(int, const char *, void *);
    if (!real)
        real = dlsym(RTLD_NEXT, "__xstat");
    IMG(path);
    return real(ver, path, st);
}

int __xstat64(int ver, const char *path, void *st)
{
    static int (*real)(int, const char *, void *);
    if (!real)
        real = dlsym(RTLD_NEXT, "__xstat64");
    IMG(path);
    return real(ver, path, st);
}

int access(const char *path, int mode)
{
    static int (*real)(const char *, int);
    if (!real)
        real = dlsym(RTLD_NEXT, "access");
    IMG(path);
    return real(path, mode);
}
