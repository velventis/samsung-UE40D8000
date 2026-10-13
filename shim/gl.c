/*
 * EGL and OpenGL ES 2.0 for exeDSP, drawn by the HD2's Adreno 200.
 *
 * exeDSP's UI (Shadow::GLRenderer, CCGfx, CCPanel) draws with GLES2 into
 * EGL pixmap surfaces over its screen buffers (Mali fbdev_pixmaps: ARGB8888
 * words in the TV's GA memory), with GA surfaces as textures through
 * EGLImages.  These calls are passed to hd2gl, an Android process on the
 * HD2 ROM's Adreno driver (see glproto.h and hd2gl/hd2gl.c), which runs
 * them and writes what was drawn back into the TV's memory.
 *
 * Here: the client side.  Object names are allocated locally, state that
 * GL queries return is kept locally, and calls are batched until a sync
 * point: glFinish/glFlush, eglWaitGL, eglMakeCurrent, eglSwapBuffers, a
 * read-back, or a GA operation (gfx.c calls hd2tv_gl_sync first and
 * reports what it wrote with hd2tv_gl_dirty).
 */
#include "hd2tv.h"
#include "glproto.h"

typedef float f32;
typedef unsigned GLenum;

#define MAXATTR 8
#define MAXUNIT 8
#define MAXUNI  96

#define GL_TRIANGLES            4
#define GL_UNSIGNED_BYTE        0x1401
#define GL_UNSIGNED_SHORT       0x1403
#define GL_UNSIGNED_INT         0x1405
#define GL_FLOAT                0x1406
#define GL_FIXED                0x140C
#define GL_SHORT                0x1402
#define GL_BYTE                 0x1400
#define GL_ALPHA                0x1906
#define GL_RGB                  0x1907
#define GL_RGBA                 0x1908
#define GL_LUMINANCE            0x1909
#define GL_LUMINANCE_ALPHA      0x190A
#define GL_BGRA_EXT             0x80E1
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_ARRAY_BUFFER         0x8892
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_TEXTURE0             0x84C0
#define GL_BLEND                0x0BE2
#define GL_SCISSOR_TEST         0x0C11
#define GL_DEPTH_TEST           0x0B71
#define GL_CULL_FACE            0x0B44

/* ------------------------------------------------------------ transport */

static uint8_t *cmd;                    /* GLP_CMD_SIZE, then the reply area */
static uint32_t pos;                    /* bytes queued */
static int cmd_w = -1, rep_r = -1;
static bool started, failed;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t sent_ctx = ~0u, sent_surf = ~0u;    /* hd2gl's current */

static long sys(long n, long a, long b, long c) { return syscall(n, a, b, c); }
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_execve 11
#define SYS_chdir 12
#define SYS_chroot 61
#define SYS_exit 1

static bool start(void)
{
    static const char *const paths[] = { "/hd2gl", "/system/bin/hd2gl" };
    int bfd, cp[2], rp[2], pfd = hd2tv_phys_fd();
    char a1[12], a2[12], a3[12], a4[12];

    if (started)
        return !failed;
    started = true;
    failed = true;
    bfd = sys(SYS_open, (long)GLP_BUF, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (bfd < 0 || ftruncate(bfd, GLP_CMD_SIZE + GLP_REPLY_SIZE) < 0) {
        hd2tv_log("gl: no %s", GLP_BUF);
        return false;
    }
    cmd = mmap(0, GLP_CMD_SIZE + GLP_REPLY_SIZE, PROT_READ | PROT_WRITE,
               MAP_SHARED, bfd, 0);
    unlink(GLP_BUF);
    if (cmd == MAP_FAILED || pipe(cp) < 0 || pipe(rp) < 0) {
        hd2tv_log("gl: no command buffer");
        return false;
    }
    snprintf(a1, sizeof(a1), "%d", cp[0]);
    snprintf(a2, sizeof(a2), "%d", rp[1]);
    snprintf(a3, sizeof(a3), "%d", bfd);
    snprintf(a4, sizeof(a4), "%d", pfd);
    char *env[] = { "LD_LIBRARY_PATH=/system/lib/egl:/system/lib",
                    "ANDROID_ROOT=/system", "ANDROID_DATA=/data", 0 };
    long pid = fork();
    if (pid == 0) {
        if (sys(SYS_chroot, (long)"/hd2/android", 0, 0) == 0 &&
            sys(SYS_chdir, (long)"/", 0, 0) == 0)
            for (int i = 0; i < 2; i++) {
                char *argv[] = { (char *)paths[i], a1, a2, a3, a4, 0 };
                sys(SYS_execve, (long)paths[i], (long)argv, (long)env);
            }
        syscall(SYS_exit, 127);
    }
    sys(SYS_close, cp[0], 0, 0);
    sys(SYS_close, rp[1], 0, 0);
    if (pid < 0) {
        hd2tv_log("gl: cannot start hd2gl");
        return false;
    }
    cmd_w = cp[1];
    rep_r = rp[0];
    /* a first round trip: hd2gl is up and has the GPU */
    uint32_t n = 0, r = 0;
    if (sys(SYS_write, cmd_w, (long)&n, 4) != 4 || sys(SYS_read, rep_r, (long)&r, 4) != 4) {
        hd2tv_log("gl: hd2gl did not start (is /hd2/android set up?)");
        return false;
    }
    hd2tv_log("gl: hd2gl %ld up", pid);
    failed = false;
    return true;
}

/* run what is queued; hd2gl's answer */
static uint32_t flush(void)
{
    uint32_t r = 0;

    if (failed || !cmd)
        return 0;
    if (sys(SYS_write, cmd_w, (long)&pos, 4) != 4 || sys(SYS_read, rep_r, (long)&r, 4) != 4) {
        hd2tv_log("gl: hd2gl is gone");
        failed = true;
    }
    pos = 0;
    return r;
}

static uint32_t *room(uint32_t words)
{
    if (pos + 4 * words > GLP_CMD_SIZE)
        flush();
    if (pos + 4 * words > GLP_CMD_SIZE)
        return 0;
    uint32_t *p = (uint32_t *)(cmd + pos);
    pos += 4 * words;
    return p;
}

/* a command with n argument words and nbytes of data */
static uint32_t *put(uint32_t op, int n, uint32_t nbytes)
{
    uint32_t dw = (nbytes + 3) / 4;
    uint32_t *p = room(2 + n + dw);
    if (!p)
        return 0;
    p[0] = op;
    p[1] = n + dw;
    return p + 2;
}

static uint32_t fbits(f32 f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ----------------------------------------------------- the client's state */

struct attr {
    bool on;
    int size;
    GLenum type;
    bool norm;
    int stride;
    uint32_t buffer;
    const uint8_t *ptr;
};

struct ctx {
    uint32_t id;
    int vp[4], sc[4];
    bool scissor, blend, depth_test, cull, sized;
    uint32_t array_buf, elem_buf, fb, rb, prog;
    int unit, unpack_align;
    uint32_t bound[MAXUNIT];
    struct attr at[MAXATTR];
};

struct surf { uint32_t id, w, h; };
struct image { uint32_t w, h, stride; uint32_t phys; };

struct prog {
    bool used;
    char attr[MAXATTR][24];
    char uni[MAXUNI][40];
    int nu;
};

static struct ctx *ctxs[GLP_MAXCTX];
static struct prog progs[GLP_MAXOBJ];
static uint8_t kinds[GLP_MAXOBJ];       /* shaders: types, for GetShaderiv */
static struct { uint8_t *data; uint32_t size; } bufs[GLP_MAXOBJ];
static uint32_t next_buf = 1, next_tex = 1, next_fb = 1, next_rb = 1, next_obj = 1;
static uint32_t next_surf = 1;

static __thread struct ctx *cur __attribute__((tls_model("initial-exec")));
static __thread struct surf *draw __attribute__((tls_model("initial-exec")));

/* GL with scalar arguments */
static void call(uint32_t fn, int n, ...)
{
    va_list ap;
    uint32_t *p;

    if (!start())
        return;
    p = put(GLP_CALL, 1 + n, 0);
    if (!p)
        return;
    p[0] = fn;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        p[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
}

/* hd2gl's current context and surface: this thread's */
static bool sel(void)
{
    uint32_t c, s;
    uint32_t *p;

    if (!cur || !start())
        return false;
    c = cur->id;
    s = draw ? draw->id : 0;
    if (c != sent_ctx || s != sent_surf) {
        if (!(p = put(GLP_MAKE_CURRENT, 2, 0)))
            return false;
        p[0] = c;
        p[1] = s;
        sent_ctx = c;
        sent_surf = s;
    }
    return true;
}

#define CALL(fn, n, ...) do { pthread_mutex_lock(&lock); if (sel()) call(fn, n, ##__VA_ARGS__); pthread_mutex_unlock(&lock); } while (0)

static void sync_locked(void)
{
    uint32_t *p;

    if (!started || failed)
        return;
    if ((p = put(GLP_SYNC, 0, 0)))
        flush();
}

void hd2tv_gl_sync(void)
{
    pthread_mutex_lock(&lock);
    if (pos || started)
        sync_locked();
    pthread_mutex_unlock(&lock);
}

void hd2tv_gl_dirty(uint32_t pa, uint32_t len)
{
    uint32_t *p;

    pthread_mutex_lock(&lock);
    if (started && !failed && (p = put(GLP_PHYS_DIRTY, 2, 0))) {
        p[0] = pa;
        p[1] = len;
    }
    pthread_mutex_unlock(&lock);
}

/* ----------------------------------------------------------------- EGL */

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext, *EGLImageKHR;
typedef int EGLint, EGLBoolean;

struct pixmap {                 /* Mali fbdev_pixmap */
    uint32_t height, width, bytes_per_pixel;
    uint8_t buffer_size, red, green, blue, alpha, luminance;
    uint16_t pad;
    uint32_t flags;
    void *data;
};

EGLDisplay eglGetDisplay(void *native) { (void)native; return (EGLDisplay)1; }

EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    (void)d;
    pthread_mutex_lock(&lock);
    start();
    pthread_mutex_unlock(&lock);
    if (major) *major = 1;
    if (minor) *minor = 4;
    return !failed;
}

EGLBoolean eglTerminate(EGLDisplay d) { (void)d; return 1; }
EGLint eglGetError(void) { return 0x3000; }
EGLBoolean eglBindAPI(unsigned api) { (void)api; return 1; }
EGLBoolean eglReleaseThread(void) { return 1; }
EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i) { (void)d; (void)i; return 1; }
const char *eglQueryString(EGLDisplay d, EGLint name)
{
    (void)d;
    return name == 0x3055 ? "EGL_KHR_image_pixmap EGL_KHR_image_base" : "1.4 hd2tv";
}

EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig *c, EGLint size, EGLint *n)
{
    (void)d;
    if (c && size > 0)
        c[0] = (EGLConfig)1;
    if (n)
        *n = 1;
    return 1;
}

EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *attr, EGLConfig *c,
                           EGLint size, EGLint *n)
{
    (void)attr;
    return eglGetConfigs(d, c, size, n);
}

EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint a, EGLint *v)
{
    (void)d; (void)c;
    switch (a) {
    case 0x3020: *v = 32; break;                /* BUFFER_SIZE */
    case 0x3021: case 0x3022: case 0x3023: case 0x3024: *v = 8; break;
    case 0x3025: *v = 16; break;                /* DEPTH */
    case 0x3033: *v = 0x7; break;               /* SURFACE_TYPE */
    case 0x3040: *v = 0x4; break;               /* RENDERABLE_TYPE: ES2 */
    default: *v = 0;
    }
    return 1;
}

EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share,
                            const EGLint *attr)
{
    struct ctx *x;
    uint32_t *p;
    int i;

    (void)d; (void)c; (void)share; (void)attr;
    pthread_mutex_lock(&lock);
    for (i = 1; i < GLP_MAXCTX && ctxs[i]; i++)
        ;
    if (i == GLP_MAXCTX || !start()) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    x = calloc(1, sizeof(*x));
    x->id = i;
    x->unpack_align = 4;
    ctxs[i] = x;
    if ((p = put(GLP_CTX_CREATE, 1, 0)))
        p[0] = i;
    pthread_mutex_unlock(&lock);
    return x;
}

EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c)
{
    struct ctx *x = c;
    uint32_t *p;

    (void)d;
    if (!x)
        return 0;
    pthread_mutex_lock(&lock);
    if ((p = put(GLP_CTX_DESTROY, 1, 0)))
        p[0] = x->id;
    if (sent_ctx == x->id)
        sent_ctx = ~0u;
    ctxs[x->id] = 0;
    pthread_mutex_unlock(&lock);
    if (cur == x)
        cur = 0;
    free(x);
    return 1;
}

EGLSurface eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, void *native,
                                  const EGLint *attr)
{
    struct pixmap *pm = native;
    struct surf *s;
    uint64_t pa = hd2tv_va2phys(pm->data);
    uint32_t *p;

    (void)d; (void)c; (void)attr;
    if (pa == ~0ull) {
        hd2tv_log("gl: pixmap %p is not in TV memory", pm->data);
        return 0;
    }
    s = calloc(1, sizeof(*s));
    s->w = pm->width;
    s->h = pm->height;
    pthread_mutex_lock(&lock);
    s->id = next_surf++;
    if (start() && (p = put(GLP_SURF_PIXMAP, 5, 0))) {
        p[0] = s->id; p[1] = s->w; p[2] = s->h; p[3] = s->w * 4; p[4] = pa;
    }
    pthread_mutex_unlock(&lock);
    hd2tv_log("gl: pixmap surface %u: %ux%u at %08llx", s->id, s->w, s->h, pa);
    return s;
}

EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, void *w, const EGLint *attr)
{
    (void)d; (void)c; (void)w; (void)attr;
    hd2tv_log("gl: eglCreateWindowSurface not done");
    return 0;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *attr)
{
    (void)d; (void)c; (void)attr;
    hd2tv_log("gl: eglCreatePbufferSurface not done");
    return 0;
}

EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s)
{
    struct surf *x = s;
    uint32_t *p;

    (void)d;
    if (!x)
        return 0;
    pthread_mutex_lock(&lock);
    sync_locked();
    if ((p = put(GLP_SURF_DESTROY, 1, 0)))
        p[0] = x->id;
    if (sent_surf == x->id)
        sent_surf = ~0u;
    pthread_mutex_unlock(&lock);
    if (draw == x)
        draw = 0;
    free(x);
    return 1;
}

EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v)
{
    struct surf *x = s;

    (void)d;
    *v = !x ? 0 : a == 0x3057 ? (EGLint)x->w : a == 0x3056 ? (EGLint)x->h : 0;
    return 1;
}

EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{
    (void)d; (void)rd;
    pthread_mutex_lock(&lock);
    sync_locked();
    pthread_mutex_unlock(&lock);
    cur = c;
    draw = dr;
    if (cur && draw && !cur->sized) {
        /* the viewport and scissor box start as the first surface */
        cur->sized = true;
        cur->vp[2] = cur->sc[2] = draw->w;
        cur->vp[3] = cur->sc[3] = draw->h;
        CALL(F_Viewport, 4, 0, 0, draw->w, draw->h);
        CALL(F_Scissor, 4, 0, 0, draw->w, draw->h);
    }
    return 1;
}

EGLContext eglGetCurrentContext(void) { return cur; }
EGLSurface eglGetCurrentSurface(EGLint w) { (void)w; return draw; }
EGLDisplay eglGetCurrentDisplay(void) { return (EGLDisplay)1; }

static void finish(void)
{
    pthread_mutex_lock(&lock);
    sync_locked();
    pthread_mutex_unlock(&lock);
}

EGLBoolean eglWaitGL(void) { finish(); return 1; }
EGLBoolean eglWaitClient(void) { finish(); return 1; }
EGLBoolean eglWaitNative(EGLint e) { (void)e; finish(); return 1; }
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) { (void)d; (void)s; finish(); return 1; }

EGLImageKHR eglCreateImageKHR(EGLDisplay d, EGLContext c, unsigned target,
                              void *buffer, const EGLint *attr)
{
    struct pixmap *pm = buffer;
    struct image *im;
    uint64_t pa;

    (void)d; (void)c; (void)attr;
    if (target != 0x30B0 || !pm) {              /* EGL_NATIVE_PIXMAP_KHR */
        hd2tv_log("gl: eglCreateImageKHR target 0x%x not done", target);
        return 0;
    }
    pa = hd2tv_va2phys(pm->data);
    if (pa == ~0ull) {
        hd2tv_log("gl: image pixmap %p is not in TV memory", pm->data);
        return 0;
    }
    im = calloc(1, sizeof(*im));
    im->w = pm->width;
    im->h = pm->height;
    im->stride = pm->width * 4;
    im->phys = pa;
    return im;
}

EGLBoolean eglDestroyImageKHR(EGLDisplay d, EGLImageKHR i)
{
    (void)d;
    free(i);
    return 1;
}

void *eglGetProcAddress(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}

/* --------------------------------------------------------- state calls */

#define C if (!cur) return

void glClearColor(f32 r, f32 g, f32 b, f32 a)
{
    CALL(F_ClearColor, 4, fbits(r), fbits(g), fbits(b), fbits(a));
}
void glClearDepthf(f32 v) { CALL(F_ClearDepthf, 1, fbits(v)); }
void glClear(unsigned bits) { CALL(F_Clear, 1, bits); }
void glViewport(int x, int y, int w, int h)
{
    C;
    cur->vp[0] = x; cur->vp[1] = y; cur->vp[2] = w; cur->vp[3] = h;
    CALL(F_Viewport, 4, x, y, w, h);
}
void glScissor(int x, int y, int w, int h)
{
    C;
    cur->sc[0] = x; cur->sc[1] = y; cur->sc[2] = w; cur->sc[3] = h;
    CALL(F_Scissor, 4, x, y, w, h);
}
static void enable(GLenum cap, bool on)
{
    C;
    switch (cap) {
    case GL_BLEND: cur->blend = on; break;
    case GL_SCISSOR_TEST: cur->scissor = on; break;
    case GL_DEPTH_TEST: cur->depth_test = on; break;
    case GL_CULL_FACE: cur->cull = on; break;
    }
    CALL(on ? F_Enable : F_Disable, 1, cap);
}
void glEnable(GLenum cap) { enable(cap, true); }
void glDisable(GLenum cap) { enable(cap, false); }
unsigned char glIsEnabled(GLenum cap)
{
    if (!cur) return 0;
    switch (cap) {
    case GL_BLEND: return cur->blend;
    case GL_SCISSOR_TEST: return cur->scissor;
    case GL_DEPTH_TEST: return cur->depth_test;
    case GL_CULL_FACE: return cur->cull;
    }
    return 0;
}
void glBlendFunc(GLenum s, GLenum d) { CALL(F_BlendFunc, 2, s, d); }
void glBlendFuncSeparate(GLenum s, GLenum d, GLenum sa, GLenum da)
{
    CALL(F_BlendFuncSeparate, 4, s, d, sa, da);
}
void glBlendEquation(GLenum m) { CALL(F_BlendEquation, 1, m); }
void glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    CALL(F_ColorMask, 4, r, g, b, a);
}
void glDepthMask(unsigned char m) { CALL(F_DepthMask, 1, m); }
void glDepthFunc(GLenum f) { CALL(F_DepthFunc, 1, f); }
void glCullFace(GLenum m) { CALL(F_CullFace, 1, m); }
void glFrontFace(GLenum m) { CALL(F_FrontFace, 1, m); }
void glHint(GLenum t, GLenum m) { CALL(F_Hint, 2, t, m); }
void glPixelStorei(GLenum p, int v)
{
    C;
    if (p == 0x0CF5)
        cur->unpack_align = v;
}
void glLineWidth(f32 w) { CALL(F_LineWidth, 1, fbits(w)); }
void glPolygonOffset(f32 a, f32 b) { CALL(F_PolygonOffset, 2, fbits(a), fbits(b)); }
void glStencilFunc(GLenum f, int r, unsigned m) { (void)f; (void)r; (void)m; }
void glStencilOp(GLenum a, GLenum b, GLenum c) { (void)a; (void)b; (void)c; }
void glStencilMask(unsigned m) { (void)m; }
void glClearStencil(int s) { (void)s; }
void glFlush(void) { finish(); }
void glFinish(void) { finish(); }
GLenum glGetError(void) { return 0; }

void glGetIntegerv(GLenum p, int *v)
{
    if (!cur) { v[0] = 0; return; }
    switch (p) {
    case 0x0D33: v[0] = 2048; break;                    /* MAX_TEXTURE_SIZE */
    case 0x0BA2: for (int i = 0; i < 4; i++) v[i] = cur->vp[i]; break;
    case 0x0C10: for (int i = 0; i < 4; i++) v[i] = cur->sc[i]; break;
    case 0x8CA6: v[0] = cur->fb; break;                 /* FRAMEBUFFER_BINDING */
    case 0x8CA7: v[0] = cur->rb; break;
    case 0x8069: v[0] = cur->bound[cur->unit]; break;   /* TEXTURE_BINDING_2D */
    case 0x8B8D: v[0] = cur->prog; break;               /* CURRENT_PROGRAM */
    case 0x8869: v[0] = MAXATTR; break;                 /* MAX_VERTEX_ATTRIBS */
    case 0x8872: case 0x8B4D: case 0x8B4C: v[0] = MAXUNIT; break;
    case 0x8DFB: case 0x8DFD: v[0] = 128; break;        /* uniform vectors */
    case 0x8DFC: v[0] = 8; break;
    case 0x84E0: v[0] = GL_TEXTURE0 + cur->unit; break;
    case 0x0CF5: v[0] = cur->unpack_align; break;
    case 0x0D3A: v[0] = 2048; v[1] = 2048; break;       /* MAX_VIEWPORT_DIMS */
    case 0x8894: v[0] = cur->array_buf; break;
    case 0x8895: v[0] = cur->elem_buf; break;
    case 0x0D50: case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: v[0] = 8; break;
    case 0x0D56: v[0] = 16; break;
    case 0x8B9A: v[0] = GL_RGBA; break;                 /* IMPL_COLOR_READ_FORMAT */
    case 0x8B9B: v[0] = GL_UNSIGNED_BYTE; break;
    default: v[0] = 0;
    }
}

void glGetFloatv(GLenum p, f32 *v)
{
    int i[4] = { 0 };
    glGetIntegerv(p, i);
    for (int k = 0; k < 4; k++) v[k] = i[k];
}

void glGetBooleanv(GLenum p, unsigned char *v)
{
    int i[4] = { 0 };
    glGetIntegerv(p, i);
    v[0] = !!i[0];
}

const unsigned char *glGetString(GLenum n)
{
    switch (n) {
    case 0x1F00: return (const unsigned char *)"Qualcomm (hd2gl)";
    case 0x1F01: return (const unsigned char *)"Adreno 200";
    case 0x1F02: return (const unsigned char *)"OpenGL ES 2.0";
    case 0x8B8C: return (const unsigned char *)"OpenGL ES GLSL ES 1.00";
    default: return (const unsigned char *)"GL_OES_EGL_image";
    }
}

/* -------------------------------------------------------------- buffers */

void glGenBuffers(int n, uint32_t *ids)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n; i++)
        ids[i] = next_buf < GLP_MAXOBJ ? next_buf++ : 0;
    pthread_mutex_unlock(&lock);
}

void glDeleteBuffers(int n, const uint32_t *ids)
{
    for (int i = 0; i < n; i++)
        if (ids[i] && ids[i] < GLP_MAXOBJ) {
            free(bufs[ids[i]].data);
            bufs[ids[i]].data = 0;
            bufs[ids[i]].size = 0;
            CALL(F_DeleteBuffer, 1, ids[i]);
        }
}

void glBindBuffer(GLenum t, uint32_t id)
{
    C;
    if (t == GL_ARRAY_BUFFER) cur->array_buf = id;
    else cur->elem_buf = id;
    CALL(F_BindBuffer, 2, t, id);
}

void glBufferData(GLenum t, long size, const void *data, GLenum usage)
{
    uint32_t id, *p;

    C;
    id = t == GL_ARRAY_BUFFER ? cur->array_buf : cur->elem_buf;
    if (!id || id >= GLP_MAXOBJ || size < 0)
        return;
    /* a copy here too: the vertex range of a draw from it */
    free(bufs[id].data);
    bufs[id].data = calloc(1, size ? size : 1);
    bufs[id].size = size;
    if (data)
        memcpy(bufs[id].data, data, size);
    pthread_mutex_lock(&lock);
    if (sel() && (p = put(GLP_BUFFER_DATA, 3, size))) {
        p[0] = t; p[1] = size; p[2] = usage;
        memcpy(p + 3, bufs[id].data, size);
    }
    pthread_mutex_unlock(&lock);
}

void glBufferSubData(GLenum t, long off, long size, const void *data)
{
    uint32_t id, *p;

    C;
    id = t == GL_ARRAY_BUFFER ? cur->array_buf : cur->elem_buf;
    if (!id || id >= GLP_MAXOBJ || off < 0 || off + size > (long)bufs[id].size)
        return;
    memcpy(bufs[id].data + off, data, size);
    pthread_mutex_lock(&lock);
    if (sel() && (p = put(GLP_BUFFER_SUBDATA, 3, size))) {
        p[0] = t; p[1] = off; p[2] = size;
        memcpy(p + 3, data, size);
    }
    pthread_mutex_unlock(&lock);
}

/* ------------------------------------------------------------- textures */

void glGenTextures(int n, uint32_t *ids)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n; i++)
        ids[i] = next_tex < GLP_MAXOBJ ? next_tex++ : 0;
    pthread_mutex_unlock(&lock);
}

void glDeleteTextures(int n, const uint32_t *ids)
{
    for (int i = 0; i < n; i++)
        if (ids[i] && ids[i] < GLP_MAXOBJ)
            CALL(F_DeleteTexture, 1, ids[i]);
}

void glActiveTexture(GLenum u)
{
    C;
    cur->unit = (u - GL_TEXTURE0) & (MAXUNIT - 1);
    CALL(F_ActiveTexture, 1, u);
}

void glBindTexture(GLenum t, uint32_t id)
{
    C;
    if (id >= GLP_MAXOBJ)
        return;
    cur->bound[cur->unit] = id;
    CALL(F_BindTexture, 2, t, id);
}

void glTexParameteri(GLenum t, GLenum p, int v) { CALL(F_TexParameteri, 3, t, p, v); }
void glTexParameterf(GLenum t, GLenum p, f32 v) { CALL(F_TexParameterf, 3, t, p, fbits(v)); }
void glTexParameteriv(GLenum t, GLenum p, const int *v) { glTexParameteri(t, p, v[0]); }
void glTexParameterfv(GLenum t, GLenum p, const f32 *v) { glTexParameterf(t, p, v[0]); }
void glGenerateMipmap(GLenum t) { CALL(F_GenerateMipmap, 1, t); }

/* one source pixel in GL's (format, type) to the TV's 0xAARRGGBB */
static uint32_t to_argb(const uint8_t *s, GLenum fmt, GLenum type)
{
    uint32_t r, g, b, a;

    if (type == GL_UNSIGNED_SHORT_5_6_5) {
        uint16_t v = s[0] | s[1] << 8;
        r = (v >> 11) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31;
        a = 255;
    } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
        uint16_t v = s[0] | s[1] << 8;
        r = (v >> 12) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17;
    } else if (type == GL_UNSIGNED_SHORT_5_5_5_1) {
        uint16_t v = s[0] | s[1] << 8;
        r = (v >> 11) * 255 / 31; g = ((v >> 6) & 31) * 255 / 31; b = ((v >> 1) & 31) * 255 / 31;
        a = v & 1 ? 255 : 0;
    } else switch (fmt) {
    case GL_RGBA: r = s[0]; g = s[1]; b = s[2]; a = s[3]; break;
    case GL_BGRA_EXT: b = s[0]; g = s[1]; r = s[2]; a = s[3]; break;
    case GL_RGB: r = s[0]; g = s[1]; b = s[2]; a = 255; break;
    case GL_LUMINANCE: r = g = b = s[0]; a = 255; break;
    case GL_LUMINANCE_ALPHA: r = g = b = s[0]; a = s[1]; break;
    case GL_ALPHA: r = g = b = 0; a = s[0]; break;
    default: r = g = b = a = 0;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

static int bytes_pp(GLenum fmt, GLenum type)
{
    if (type != GL_UNSIGNED_BYTE)
        return 2;
    switch (fmt) {
    case GL_RGBA: case GL_BGRA_EXT: return 4;
    case GL_RGB: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;
    }
}

/* rows [y, y + h) of an upload, converted; in pieces that fit */
static void upload(bool sub, int level, int x, int y, int w, int h, GLenum fmt,
                   GLenum type, const void *data)
{
    int bpp = bytes_pp(fmt, type), al = cur->unpack_align;
    int pitch = (w * bpp + al - 1) / al * al;
    int rows = w ? (int)((GLP_CMD_SIZE / 2) / (w * 4)) : h;

    if (!sub) {
        /* the storage first, then the pixels as sub-images */
        uint32_t *p = put(GLP_TEX_IMAGE, 3, 0);
        if (!p)
            return;
        p[0] = level; p[1] = w; p[2] = h;
        if (!data)
            return;
    }
    for (int r0 = 0; r0 < h; r0 += rows) {
        int n = h - r0 < rows ? h - r0 : rows;
        uint32_t *p = put(GLP_TEX_SUBIMAGE, 5, (uint32_t)w * n * 4);
        if (!p)
            return;
        p[0] = level; p[1] = x; p[2] = y + r0; p[3] = w; p[4] = n;
        for (int j = 0; j < n; j++) {
            const uint8_t *s = (const uint8_t *)data + (size_t)(r0 + j) * pitch;
            uint32_t *d = p + 5 + j * w;
            if (fmt == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE)
                memcpy(d, s, w * 4);
            else
                for (int i = 0; i < w; i++)
                    d[i] = to_argb(s + i * bpp, fmt, type);
        }
    }
}

void glTexImage2D(GLenum target, int level, int ifmt, int w, int h, int border,
                  GLenum fmt, GLenum type, const void *data)
{
    (void)target; (void)ifmt; (void)border;
    C;
    pthread_mutex_lock(&lock);
    if (sel())
        upload(false, level, 0, 0, w, h, fmt, type, data);
    pthread_mutex_unlock(&lock);
}

void glTexSubImage2D(GLenum target, int level, int x, int y, int w, int h,
                     GLenum fmt, GLenum type, const void *data)
{
    (void)target;
    C;
    pthread_mutex_lock(&lock);
    if (sel() && data)
        upload(true, level, x, y, w, h, fmt, type, data);
    pthread_mutex_unlock(&lock);
}

void glCompressedTexImage2D(GLenum target, int level, GLenum ifmt, int w, int h,
                            int border, int size, const void *data)
{
    (void)target; (void)border; (void)size; (void)data;
    hd2tv_log("gl: glCompressedTexImage2D 0x%x not done", ifmt);
    glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
}

void glCompressedTexSubImage2D(GLenum target, int level, int x, int y, int w,
                               int h, GLenum fmt, int size, const void *data)
{
    (void)target; (void)level; (void)x; (void)y; (void)w; (void)h; (void)size; (void)data;
    hd2tv_log("gl: glCompressedTexSubImage2D 0x%x not done", fmt);
}

void glEGLImageTargetTexture2DOES(GLenum target, void *image)
{
    struct image *im = image;
    uint32_t *p;

    (void)target;
    C;
    if (!im)
        return;
    pthread_mutex_lock(&lock);
    if (sel() && (p = put(GLP_TEX_PHYS, 5, 0))) {
        p[0] = cur->bound[cur->unit];
        p[1] = im->w; p[2] = im->h; p[3] = im->stride; p[4] = im->phys;
    }
    pthread_mutex_unlock(&lock);
}

/* --------------------------------------------------------- framebuffers */

void glGenFramebuffers(int n, uint32_t *ids)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n; i++)
        ids[i] = next_fb < GLP_MAXOBJ ? next_fb++ : 0;
    pthread_mutex_unlock(&lock);
}

void glDeleteFramebuffers(int n, const uint32_t *ids)
{
    for (int i = 0; i < n; i++)
        if (ids[i] && ids[i] < GLP_MAXOBJ) {
            if (cur && cur->fb == ids[i])
                cur->fb = 0;
            CALL(F_DeleteFramebuffer, 1, ids[i]);
        }
}

void glBindFramebuffer(GLenum t, uint32_t id)
{
    C;
    cur->fb = id < GLP_MAXOBJ ? id : 0;
    CALL(F_BindFramebuffer, 2, t, cur->fb);
}

void glFramebufferTexture2D(GLenum t, GLenum att, GLenum tt, uint32_t tex, int level)
{
    CALL(F_FramebufferTexture2D, 5, t, att, tt, tex, level);
}

void glFramebufferRenderbuffer(GLenum t, GLenum att, GLenum rt, uint32_t rb)
{
    CALL(F_FramebufferRenderbuffer, 4, t, att, rt, rb);
}

GLenum glCheckFramebufferStatus(GLenum t) { (void)t; return 0x8CD5; }

void glGenRenderbuffers(int n, uint32_t *ids)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n; i++)
        ids[i] = next_rb < GLP_MAXOBJ ? next_rb++ : 0;
    pthread_mutex_unlock(&lock);
}

void glDeleteRenderbuffers(int n, const uint32_t *ids)
{
    for (int i = 0; i < n; i++)
        if (ids[i] && ids[i] < GLP_MAXOBJ)
            CALL(F_DeleteRenderbuffer, 1, ids[i]);
}

void glBindRenderbuffer(GLenum t, uint32_t id)
{
    C;
    cur->rb = id;
    CALL(F_BindRenderbuffer, 2, t, id);
}

void glRenderbufferStorage(GLenum t, GLenum fmt, int w, int h)
{
    CALL(F_RenderbufferStorage, 4, t, fmt, w, h);
}

void glReadPixels(int x, int y, int w, int h, GLenum fmt, GLenum type, void *data)
{
    uint32_t *p, *src;
    uint8_t *d = data;

    C;
    if (type != GL_UNSIGNED_BYTE || (uint32_t)w * h * 4 > GLP_REPLY_SIZE)
        return;
    pthread_mutex_lock(&lock);
    if (sel() && (p = put(GLP_READ_PIXELS, 4, 0))) {
        p[0] = x; p[1] = y; p[2] = w; p[3] = h;
        flush();
        src = (uint32_t *)(cmd + GLP_CMD_SIZE);
        for (int i = 0; i < w * h; i++) {
            uint32_t c = src[i];
            uint8_t *o = d + i * (fmt == GL_RGB ? 3 : 4);
            o[0] = c >> 16; o[1] = c >> 8; o[2] = c;
            if (fmt != GL_RGB)
                o[3] = c >> 24;
        }
    }
    pthread_mutex_unlock(&lock);
}

/* ------------------------------------------------------ shaders, programs */

uint32_t glCreateShader(GLenum type)
{
    uint32_t id;

    pthread_mutex_lock(&lock);
    id = next_obj < GLP_MAXOBJ ? next_obj++ : 0;
    pthread_mutex_unlock(&lock);
    kinds[id] = type == GL_FRAGMENT_SHADER;
    return id;
}

void glDeleteShader(uint32_t id) { CALL(F_DeleteShader, 1, id); }

static bool has(const void *p, size_t n, const char *s)
{
    size_t l = strlen(s);
    for (size_t i = 0; i + l <= n; i++)
        if (!memcmp((const char *)p + i, s, l))
            return true;
    return false;
}

static enum glp_kind classify(bool frag, const void *p, size_t n)
{
    if (has(p, n, "alphaConstant"))
        return KIND_2D_FRAG;
    if (has(p, n, "mVPMatrix"))
        return KIND_3D_VERT;
    if (has(p, n, "mObjectType") || (frag && has(p, n, "LocalColor")))
        return KIND_3D_FRAG;
    if (!frag && has(p, n, "inVertex"))
        return KIND_2D_VERT;
    return KIND_UNKNOWN;
}

static void shader(uint32_t id, enum glp_kind kind)
{
    uint32_t *p;

    if (kind == KIND_UNKNOWN)
        hd2tv_log("gl: shader %u not recognised", id);
    pthread_mutex_lock(&lock);
    if (start() && (p = put(GLP_SHADER, 3, 0))) {
        p[0] = id; p[1] = kinds[id] ? GL_FRAGMENT_SHADER : 0x8B31; p[2] = kind;
    }
    pthread_mutex_unlock(&lock);
}

void glShaderBinary(int n, const uint32_t *ids, GLenum fmt, const void *bin, int len)
{
    (void)fmt;
    for (int i = 0; i < n; i++)
        if (ids[i] && ids[i] < GLP_MAXOBJ)
            shader(ids[i], classify(kinds[ids[i]], bin, len));
}

void glShaderSource(uint32_t id, int n, const char *const *src, const int *len)
{
    enum glp_kind k = KIND_UNKNOWN;

    if (!id || id >= GLP_MAXOBJ)
        return;
    for (int i = 0; i < n && k == KIND_UNKNOWN; i++)
        k = classify(kinds[id], src[i], len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]));
    shader(id, k);
}

void glCompileShader(uint32_t id) { (void)id; }
void glReleaseShaderCompiler(void) {}

void glGetShaderiv(uint32_t id, GLenum p, int *v)
{
    *v = p == 0x8B81 ? 1 : p == 0x8B4F ? (int)(kinds[id < GLP_MAXOBJ ? id : 0] ?
                                                GL_FRAGMENT_SHADER : 0x8B31) : 0;
}

void glGetShaderInfoLog(uint32_t id, int max, int *len, char *log)
{
    (void)id;
    if (len) *len = 0;
    if (max > 0) log[0] = 0;
}

uint32_t glCreateProgram(void)
{
    uint32_t id;

    pthread_mutex_lock(&lock);
    id = next_obj < GLP_MAXOBJ ? next_obj++ : 0;
    memset(&progs[id], 0, sizeof(progs[id]));
    progs[id].used = true;
    if (start())
        call(F_CreateProgram, 1, id);
    pthread_mutex_unlock(&lock);
    return id;
}

void glDeleteProgram(uint32_t id) { CALL(F_DeleteProgram, 1, id); }

void glAttachShader(uint32_t p, uint32_t s)
{
    pthread_mutex_lock(&lock);
    if (start())
        call(F_AttachShader, 2, p, s);
    pthread_mutex_unlock(&lock);
}

void glDetachShader(uint32_t p, uint32_t s) { (void)p; (void)s; }

void glBindAttribLocation(uint32_t pr, uint32_t idx, const char *name)
{
    uint32_t *p, n = strlen(name) + 1;

    if (!pr || pr >= GLP_MAXOBJ || idx >= MAXATTR || n > 24)
        return;
    strcpy(progs[pr].attr[idx], name);
    pthread_mutex_lock(&lock);
    if (start() && (p = put(GLP_BIND_ATTRIB, 2, n))) {
        p[0] = pr; p[1] = idx;
        memcpy(p + 2, name, n);
    }
    pthread_mutex_unlock(&lock);
}

static const char *const default_attr[] = { "inVertex", "inTexCoord", "inNormal", "inColor" };

void glLinkProgram(uint32_t pr)
{
    if (!pr || pr >= GLP_MAXOBJ)
        return;
    /* attributes not bound get the first free locations, also in hd2gl */
    for (unsigned n = 0; n < sizeof(default_attr) / sizeof(default_attr[0]); n++) {
        bool bound = false;
        for (int i = 0; i < MAXATTR; i++)
            bound |= !strcmp(progs[pr].attr[i], default_attr[n]);
        for (int i = 0; i < MAXATTR && !bound; i++)
            if (!progs[pr].attr[i][0]) {
                glBindAttribLocation(pr, i, default_attr[n]);
                bound = true;
            }
    }
    pthread_mutex_lock(&lock);
    if (start())
        call(F_LinkProgram, 1, pr);
    pthread_mutex_unlock(&lock);
}

void glGetProgramiv(uint32_t p, GLenum q, int *v)
{
    (void)p;
    *v = q == 0x8B82 || q == 0x8B80 ? 1 : 0;      /* LINK/VALIDATE_STATUS */
}

void glGetProgramInfoLog(uint32_t id, int max, int *len, char *log)
{
    (void)id;
    if (len) *len = 0;
    if (max > 0) log[0] = 0;
}

void glValidateProgram(uint32_t p) { (void)p; }

void glUseProgram(uint32_t p)
{
    C;
    cur->prog = p < GLP_MAXOBJ ? p : 0;
    CALL(F_UseProgram, 1, cur->prog);
}

int glGetAttribLocation(uint32_t p, const char *name)
{
    if (!p || p >= GLP_MAXOBJ)
        return -1;
    for (int i = 0; i < MAXATTR; i++)
        if (!strcmp(progs[p].attr[i], name))
            return i;
    return -1;
}

int glGetUniformLocation(uint32_t pr, const char *name)
{
    struct prog *g;
    uint32_t *p, n = strlen(name) + 1;
    int i;

    if (!pr || pr >= GLP_MAXOBJ || n > 40)
        return -1;
    g = &progs[pr];
    pthread_mutex_lock(&lock);
    for (i = 0; i < g->nu && strcmp(g->uni[i], name); i++)
        ;
    if (i == g->nu && g->nu < MAXUNI) {
        strcpy(g->uni[i], name);
        g->nu++;
        if (start() && (p = put(GLP_UNIFORM_NAME, 2, n))) {
            p[0] = pr; p[1] = i;
            memcpy(p + 2, name, n);
        }
    }
    pthread_mutex_unlock(&lock);
    return i < MAXUNI ? i : -1;
}

static void uniform(int loc, int kind, int n, int count, const void *v)
{
    uint32_t *p;

    if (loc < 0 || count <= 0)
        return;
    pthread_mutex_lock(&lock);
    if (sel() && (p = put(GLP_UNIFORM, 4, 4 * n * count))) {
        p[0] = loc; p[1] = kind; p[2] = n; p[3] = count;
        memcpy(p + 4, v, 4 * n * count);
    }
    pthread_mutex_unlock(&lock);
}

void glUniform1f(int l, f32 a) { uniform(l, 0, 1, 1, &a); }
void glUniform2f(int l, f32 a, f32 b) { f32 v[] = { a, b }; uniform(l, 0, 2, 1, v); }
void glUniform3f(int l, f32 a, f32 b, f32 c) { f32 v[] = { a, b, c }; uniform(l, 0, 3, 1, v); }
void glUniform4f(int l, f32 a, f32 b, f32 c, f32 d) { f32 v[] = { a, b, c, d }; uniform(l, 0, 4, 1, v); }
void glUniform1fv(int l, int n, const f32 *v) { uniform(l, 0, 1, n, v); }
void glUniform2fv(int l, int n, const f32 *v) { uniform(l, 0, 2, n, v); }
void glUniform3fv(int l, int n, const f32 *v) { uniform(l, 0, 3, n, v); }
void glUniform4fv(int l, int n, const f32 *v) { uniform(l, 0, 4, n, v); }
void glUniform1i(int l, int a) { uniform(l, 1, 1, 1, &a); }
void glUniform2i(int l, int a, int b) { int v[] = { a, b }; uniform(l, 1, 2, 1, v); }
void glUniform3i(int l, int a, int b, int c) { int v[] = { a, b, c }; uniform(l, 1, 3, 1, v); }
void glUniform4i(int l, int a, int b, int c, int d) { int v[] = { a, b, c, d }; uniform(l, 1, 4, 1, v); }
void glUniform1iv(int l, int n, const int *v) { uniform(l, 1, 1, n, v); }
void glUniform2iv(int l, int n, const int *v) { uniform(l, 1, 2, n, v); }
void glUniform3iv(int l, int n, const int *v) { uniform(l, 1, 3, n, v); }
void glUniform4iv(int l, int n, const int *v) { uniform(l, 1, 4, n, v); }
void glUniformMatrix2fv(int l, int n, unsigned char t, const f32 *v) { (void)t; uniform(l, 2, 4, n, v); }
void glUniformMatrix3fv(int l, int n, unsigned char t, const f32 *v) { (void)t; uniform(l, 3, 9, n, v); }
void glUniformMatrix4fv(int l, int n, unsigned char t, const f32 *v) { (void)t; uniform(l, 4, 16, n, v); }

/* ------------------------------------------------------------- vertices */

void glEnableVertexAttribArray(uint32_t i)
{
    C;
    if (i < MAXATTR) cur->at[i].on = true;
    CALL(F_EnableVertexAttribArray, 1, i);
}

void glDisableVertexAttribArray(uint32_t i)
{
    C;
    if (i < MAXATTR) cur->at[i].on = false;
    CALL(F_DisableVertexAttribArray, 1, i);
}

void glVertexAttribPointer(uint32_t i, int size, GLenum type, unsigned char norm,
                           int stride, const void *ptr)
{
    C;
    if (i >= MAXATTR)
        return;
    struct attr *a = &cur->at[i];
    a->size = size;
    a->type = type;
    a->norm = norm;
    a->stride = stride;
    a->buffer = cur->array_buf;
    a->ptr = ptr;
}

static void vattr(uint32_t i, f32 x, f32 y, f32 z, f32 w)
{
    CALL(F_VertexAttrib4f, 5, i, fbits(x), fbits(y), fbits(z), fbits(w));
}
void glVertexAttrib1f(uint32_t i, f32 x) { vattr(i, x, 0, 0, 1); }
void glVertexAttrib2f(uint32_t i, f32 x, f32 y) { vattr(i, x, y, 0, 1); }
void glVertexAttrib3f(uint32_t i, f32 x, f32 y, f32 z) { vattr(i, x, y, z, 1); }
void glVertexAttrib4f(uint32_t i, f32 x, f32 y, f32 z, f32 w) { vattr(i, x, y, z, w); }
void glVertexAttrib1fv(uint32_t i, const f32 *v) { vattr(i, v[0], 0, 0, 1); }
void glVertexAttrib2fv(uint32_t i, const f32 *v) { vattr(i, v[0], v[1], 0, 1); }
void glVertexAttrib3fv(uint32_t i, const f32 *v) { vattr(i, v[0], v[1], v[2], 1); }
void glVertexAttrib4fv(uint32_t i, const f32 *v) { vattr(i, v[0], v[1], v[2], v[3]); }

static int type_size(GLenum t)
{
    return t == GL_FLOAT || t == GL_FIXED || t == GL_UNSIGNED_INT ? 4 :
           t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : 1;
}

/* the arrays for vertices [first, first + n): client ones copied */
static bool arrays(uint32_t first, uint32_t n)
{
    for (int i = 0; i < MAXATTR; i++) {
        struct attr *a = &cur->at[i];
        uint32_t *p;
        if (!a->on)
            continue;
        int es = type_size(a->type) * a->size, st = a->stride ? a->stride : es;
        if (a->buffer) {
            if (!(p = put(GLP_ATTRIB, 9, 0)))
                return false;
            p[5] = a->buffer;
            p[6] = (uintptr_t)a->ptr;
            p[7] = 0;
            p[8] = 0;
        } else {
            uint32_t bytes = n ? (n - 1) * st + es : 0;
            if (!a->ptr || !(p = put(GLP_ATTRIB, 9, bytes)))
                return false;
            p[5] = GLP_INLINE;
            p[6] = 0;
            p[7] = first;
            p[8] = bytes;
            memcpy(p + 9, a->ptr + (size_t)first * st, bytes);
        }
        p[0] = i; p[1] = a->size; p[2] = a->type; p[3] = a->norm; p[4] = st;
    }
    return true;
}

void glDrawArrays(GLenum mode, int first, int count)
{
    uint32_t *p;

    C;
    if (count <= 0 || first < 0)
        return;
    pthread_mutex_lock(&lock);
    if (sel() && arrays(first, count) && (p = put(GLP_DRAW_ARRAYS, 3, 0))) {
        p[0] = mode; p[1] = first; p[2] = count;
    }
    pthread_mutex_unlock(&lock);
}

void glDrawElements(GLenum mode, int count, GLenum type, const void *idx)
{
    const uint8_t *ix = idx;
    uint32_t lo = ~0u, hi = 0, *p = 0, isz = type_size(type);

    C;
    if (count <= 0)
        return;
    if (cur->elem_buf) {
        if (cur->elem_buf >= GLP_MAXOBJ || !bufs[cur->elem_buf].data ||
            (uintptr_t)idx + count * isz > bufs[cur->elem_buf].size)
            return;
        ix = bufs[cur->elem_buf].data + (uintptr_t)idx;
    }
    if (!ix)
        return;
    for (int i = 0; i < count; i++) {
        uint32_t v = type == GL_UNSIGNED_BYTE ? ix[i] :
                     type == GL_UNSIGNED_SHORT ? ((const uint16_t *)ix)[i] :
                     ((const uint32_t *)ix)[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    pthread_mutex_lock(&lock);
    if (sel() && arrays(lo, hi - lo + 1)) {
        if (cur->elem_buf && (p = put(GLP_DRAW_ELEMENTS, 5, 0))) {
            p[3] = cur->elem_buf;
            p[4] = (uintptr_t)idx;
        } else if (!cur->elem_buf && (p = put(GLP_DRAW_ELEMENTS, 5, count * isz))) {
            p[3] = GLP_INLINE;
            p[4] = 0;
            memcpy(p + 5, ix, count * isz);
        }
        if (p) {
            p[0] = mode; p[1] = count; p[2] = type;
        }
    }
    pthread_mutex_unlock(&lock);
}
