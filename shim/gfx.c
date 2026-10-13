/*
 * samdrv's gfx device: the four graphics planes (OSD windows) and the GA,
 * the 2D accelerator that draws into them.  The driver's state (set by
 * the Set ioctls, read back by the Get ones) is kept per plane; the GA's
 * fills and blits are done by the CPU.
 *
 * Plane memory as devGfx_InitInfo lays it out: a back buffer at the
 * plane's base and a front buffer (the one shown) after it.
 */
#include "hd2tv.h"

struct sam_arg { void *buf; int32_t ret; };

/* the driver's pixel formats (devGfx_SetRes): bytes per pixel */
static const uint8_t fmt_bpp[22] = {
    1, 1, 1, 4, 1, 1, 1, 1, 2, 2, 2, 4, 2, 2, 2, 1, 4, 2, 2, 1, 1, 1,
};

struct plane {
    bool init, on;
    uint32_t res[8];            /* SetRes: ..., [3] width, [4] height, [5] format */
    uint32_t back, front;       /* physical */
    uint32_t stereo[3], paired[2], scaler[6], dblsrc, view[2], osgpos[4];
    uint32_t pivot, cot_mode, cache, zorder, osginfo[25], cursor[2], blend;
    bool shown;                 /* an OSD update has copied to the front */
};
static struct plane planes[4];
/* the planes' memory (memcfg ids: base, then size) */
static const uint8_t plane_mem[4] = { 0x28, 0x2a, 0x2c, 0x30 };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void plane_init(int p)
{
    struct plane *s = &planes[p];
    uint32_t base = hd2tv_memcfg(plane_mem[p]), size = hd2tv_memcfg(plane_mem[p] + 1);

    if (s->init)
        return;
    s->init = true;
    s->back = base;
    s->front = p == 0 ? base + 0x3f5000 : p == 1 ? base + 0x7e9000 : base + size / 2;
    s->res[3] = 1920;
    s->res[4] = 1080;
    s->res[5] = 0xb;
    s->stereo[1] = p;
    s->paired[0] = p;
    s->zorder = p;
    s->osgpos[2] = 1920;
    s->osgpos[3] = 1080;
}

static uint32_t bpp(const struct plane *s)
{
    return s->res[5] < sizeof(fmt_bpp) ? fmt_bpp[s->res[5]] : 4;
}

/* for fb.c: the planes that are on, bottom first (plane 0, the UI's, on
 * top); returns how many */
int hd2tv_gfx_planes(struct hd2tv_view *v, int max)
{
    static const int order[3] = { 2, 1, 0 };
    int n = 0;

    pthread_mutex_lock(&lock);
    for (int i = 0; i < 3 && n < max; i++) {
        struct plane *s = &planes[order[i]];
        if (!s->init || !s->on || !s->res[3] || !s->res[4] || bpp(s) != 4)
            continue;
        v[n].pa = s->shown ? s->front : s->back;
        v[n].w = s->res[3];
        v[n].h = s->res[4];
        v[n].stride = (s->res[3] * 4 + 7) & ~7u;
        n++;
    }
    pthread_mutex_unlock(&lock);
    return n;
}

/* ---------------------------------------------------------------- GA */

/* a mapped rectangle of TV memory: pixel (x, y) at px(r, x, y) */
struct rect { uint8_t *map; size_t len; uint8_t *base; uint32_t pitch; };

static bool rect_map(struct rect *r, uint32_t pa, uint32_t pitch, uint32_t x,
                     uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t start = pa + y * pitch + x * 4;
    uint32_t len = (h - 1) * pitch + w * 4;

    if (!pa || !w || !h || !pitch || w > 4096 || h > 4096)
        return false;
    r->map = hd2tv_phys(start & ~0xfffu, len + (start & 0xfff));
    if (!r->map)
        return false;
    r->len = len + (start & 0xfff);
    r->base = r->map + (start & 0xfff);
    r->pitch = pitch;
    return true;
}

static void rect_unmap(struct rect *r)
{
    munmap(r->map, r->len);
}

#define ROW(r, y) ((uint32_t *)((r)->base + (size_t)(y) * (r)->pitch))

/* FillRect { dst, pitch, format, x, y, w, h, color } */
static void ga_fill(const uint32_t *f)
{
    struct rect d;

    if (!rect_map(&d, f[0], f[1], f[3], f[4], f[5], f[6]))
        return;
    for (uint32_t y = 0; y < f[6]; y++) {
        uint32_t *o = ROW(&d, y);
        for (uint32_t x = 0; x < f[5]; x++)
            o[x] = f[7];
    }
    rect_unmap(&d);
}

/* BltCopy { format, src, spitch, sx, sy, w, h, dst, dpitch, dx, dy } */
static void ga_copy(const uint32_t *b)
{
    struct rect sr, d;

    if (!rect_map(&sr, b[1], b[2], b[3], b[4], b[5], b[6]))
        return;
    if (rect_map(&d, b[7], b[8], b[9], b[10], b[5], b[6])) {
        for (uint32_t y = 0; y < b[6]; y++)
            memmove(ROW(&d, y), ROW(&sr, y), b[5] * 4);
        rect_unmap(&d);
    }
    rect_unmap(&sr);
}

/* ARGB8888 src over dst, src alpha scaled by ga (0..255) */
static uint32_t over(uint32_t s, uint32_t d, uint32_t ga)
{
    uint32_t sa = (s >> 24) * ga / 255, da = d >> 24;
    uint32_t oa, r, g, b;

    if (sa == 255)
        return s | 0xff000000u;
    if (sa == 0)
        return d;
    oa = sa + da * (255 - sa) / 255;
    if (!oa)
        return 0;
    r = (((s >> 16) & 0xff) * sa + ((d >> 16) & 0xff) * da * (255 - sa) / 255) / oa;
    g = (((s >> 8) & 0xff) * sa + ((d >> 8) & 0xff) * da * (255 - sa) / 255) / oa;
    b = ((s & 0xff) * sa + (d & 0xff) * da * (255 - sa) / 255) / oa;
    return oa << 24 | r << 16 | g << 8 | b;
}

/*
 * BltROP { mode, format, src, spitch, sx, sy, w, h, dst, dpitch, dx, dy,
 *          rule, colorkey, alpha, ... }
 * Rules (SdROPMode_k): 2 colour keyed copy, 0xb constant alpha, 0xc fill;
 * the others are drawn as "source over".
 */
static void ga_rop(uint32_t *b, uint32_t back)
{
    struct rect sr, d;
    uint32_t w = b[6], h = b[7], rule = b[12], ga = b[14] & 0xff;

    if (!b[8])
        b[8] = back;
    if (!rect_map(&d, b[8], b[9], b[10], b[11], w, h))
        return;
    if (rule == 0xc) {
        for (uint32_t y = 0; y < h; y++)
            memset(ROW(&d, y), 0, w * 4);
        rect_unmap(&d);
        return;
    }
    if (!rect_map(&sr, b[2], b[3], b[4], b[5], w, h)) {
        rect_unmap(&d);
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *i = ROW(&sr, y);
        uint32_t *o = ROW(&d, y);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = i[x];
            if (rule == 2)
                o[x] = c == b[13] ? o[x] : c;
            else if (rule == 0xb)
                o[x] = over((c & 0xffffff) | ga << 24, o[x], 255);
            else
                o[x] = over(c, o[x], ga ? ga : 255);
        }
    }
    rect_unmap(&sr);
    rect_unmap(&d);
}

/* show what was drawn: copy the back buffer's rect to the front buffer */
static void osd_update(struct plane *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t pitch = (s->res[3] * bpp(s) + 7) & ~7u;
    struct rect sr, d;

    if (s->back == s->front || !w || !h)
        return;
    if (x + w > s->res[3]) w = s->res[3] > x ? s->res[3] - x : 0;
    if (y + h > s->res[4]) h = s->res[4] > y ? s->res[4] - y : 0;
    if (!w || !h || !rect_map(&sr, s->back, pitch, x * bpp(s) / 4, y, w * bpp(s) / 4, h))
        return;
    if (rect_map(&d, s->front, pitch, x * bpp(s) / 4, y, w * bpp(s) / 4, h)) {
        for (uint32_t i = 0; i < h; i++)
            memcpy(ROW(&d, i), ROW(&sr, i), w * bpp(s));
        rect_unmap(&d);
    }
    rect_unmap(&sr);
    s->shown = true;
}

static void logw(const char *what, int p, const uint32_t *w, int n)
{
    char buf[400];
    int k = snprintf(buf, sizeof(buf), "gfx%d %s:", p, what);
    for (int i = 0; i < n && k < (int)sizeof(buf) - 10; i++)
        k += snprintf(buf + k, sizeof(buf) - k, " %x", w[i]);
    hd2tv_log("%s", buf);
}

/* log the first few calls of each kind */
static bool sample(unsigned long req)
{
    static uint8_t count[256];
    return (hd2tv_log_mask & 0x40) || count[req & 0xff]++ < 6;
}

static void copy_out(void *dst, const void *src, size_t n)
{
    if (dst)
        memcpy(dst, src, n);
}

int hd2_gfx_ioctl(int unit, unsigned long req, void *arg)
{
    struct sam_arg *a = arg;
    uint32_t *io = a ? a->buf : 0;
    int p = unit & 3;
    struct plane *s = &planes[p];

    pthread_mutex_lock(&lock);
    plane_init(p);
    switch (req) {
    /* ---- memory */
    case 0x470a: io[0] = s->back; break;                /* GetBackBufMemBase */
    case 0x470b: io[0] = s->front; break;               /* GetFrontBufMemBase */
    case 0x470c: s->front = io[0]; break;               /* SetFrontBufMemBase { addr, ? } */
    case 0x470e: io[0] = s->back; break;                /* 2nd image back */
    case 0x470f: io[0] = s->front; break;
    case 0x4711:                                        /* plane base, size */
        io[0] = hd2tv_memcfg(plane_mem[p]);
        io[1] = hd2tv_memcfg(plane_mem[p] + 1);
        break;
    case 0x4712:                                        /* ROP buffer (plane 3) */
        io[0] = p == 3 ? hd2tv_memcfg(0x7e) : 0;
        io[1] = p == 3 ? hd2tv_memcfg(0x7f) : 0;
        break;
    case 0x47aa: io[0] = hd2tv_memcfg(0x26); io[1] = hd2tv_memcfg(0x27); break;
    case 0x47ac: io[0] = hd2tv_memcfg(0x26); break;     /* cursor memory */
    case 0x474e: io[0] = p; break;                      /* GetPlaneInstNum */

    /* ---- resolution and window */
    case 0x471e:                                        /* SetRes */
        memcpy(s->res, io, 32);
        s->shown = false;
        logw("SetRes", p, s->res, 8);
        break;
    case 0x471f: memcpy(io, s->res, 32); break;         /* GetResInfo */
    case 0x4720:                                        /* GetRes { w, h, Bpp } */
        io[0] = s->res[3]; io[1] = s->res[4]; io[2] = bpp(s);
        break;
    case 0x4736: io[0] = s->res[3]; io[1] = s->res[4]; break;   /* GetPlaneSize */
    case 0x4722: memcpy(s->scaler, io, 24); break;
    case 0x4723: memcpy(io, s->scaler, 24); break;
    case 0x4724:                                        /* SetOsgOnOff */
        s->on = io[0];
        hd2tv_log("gfx%d osg %s", p, s->on ? "on" : "off");
        break;
    case 0x4725: io[0] = s->on; break;
    case 0x4726: memcpy(s->stereo, io, 12); break;      /* SetStereoScopicMode */
    case 0x4727: io[0] = s->stereo[0]; break;
    case 0x4729: s->dblsrc = io[0]; break;
    case 0x472a: io[0] = s->dblsrc; break;
    case 0x472b: memcpy(s->view, io, 8); break;         /* MoveViewPointerInRect */
    case 0x472c: memcpy(io, s->view, 8); break;
    case 0x472d: memcpy(s->paired, io, 8); break;       /* SetPairedID */
    case 0x472e: io[0] = s->paired[0]; break;
    case 0x4731: io[0] = 0; break;
    case 0x4734: s->blend = io[0]; break;
    case 0x4737: memcpy(io, s->osginfo, 100); break;    /* OsgInfo */
    case 0x4738: memcpy(s->osginfo, io, 100); break;
    case 0x473c: s->pivot = io[0]; break;
    case 0x473d: io[0] = s->pivot; break;
    case 0x473e: io[0] = s->zorder; break;
    case 0x4797: s->zorder = io[0]; break;
    case 0x473f: memcpy(s->osgpos, io, 16); break;      /* OsgPos */
    case 0x4740: memcpy(io, s->osgpos, 16); break;
    case 0x4743: s->cache = io[0]; break;
    case 0x4744: case 0x4745: io[0] = s->cache; break;
    case 0x4741: case 0x4746: case 0x4747: case 0x4748: case 0x4749:
    case 0x474b: case 0x4782:
        io[0] = 0;
        break;
    case 0x474c: s->cot_mode = io[0]; break;            /* CopyOrToggleMode */
    case 0x474d: io[0] = s->cot_mode; break;
    case 0x47ae: memcpy(s->cursor, io, 8); break;       /* cursor position */
    case 0x47af: memcpy(io, s->cursor, 8); break;
    case 0x4796:                                        /* WaitVSync */
        pthread_mutex_unlock(&lock);
        usleep(16000);
        pthread_mutex_lock(&lock);
        break;

    /* ---- GA: draw */
    case 0x4764: case 0x4766:                           /* UpdateOsd(_Toggle) { x, y, w, h } */
        if (sample(req)) logw(req == 0x4764 ? "UpdateOsd" : "UpdateOsd_Toggle", p, io, 4);
        osd_update(s, io[0], io[1], io[2], io[3]);
        break;
    case 0x4765: case 0x4768:                           /* { src rect, dst rect } */
        if (sample(req)) logw(req == 0x4765 ? "UpdateOsd_full" : "UpdateOsd_Toggle_full", p, io, 8);
        osd_update(s, io[4], io[5], io[6], io[7]);
        break;
    case 0x4767:
        if (sample(req)) logw("UpdateOsd_Toggle_new", p, io, 9);
        osd_update(s, io[0], io[1], io[2], io[3]);
        break;
    case 0x476e:
        if (sample(req)) logw("FillRect", p, io, 8);
        ga_fill(io);
        break;
    case 0x476f:
        if (sample(req)) logw("BltROP", p, io, 21);
        ga_rop(io, s->back);
        break;
    case 0x4770:
        if (sample(req)) logw("BltCopy", p, io, 11);
        ga_copy(io);
        break;
    case 0x4771: if (sample(req)) logw("BltBackBuffer", p, io, 6); break;
    case 0x4778: if (sample(req)) logw("GSCL_Scale", p, io, 9); break;
    case 0x4779: if (sample(req)) logw("GSCL_FillRect", p, io, 8); break;
    case 0x477a: if (sample(req)) logw("GSCL_BltCopy", p, io, 9); break;
    case 0x4798: if (sample(req)) logw("ConvertFormat", p, io, 13); break;
    }
    pthread_mutex_unlock(&lock);
    if (a)
        a->ret = 0;
    return 0;
}
