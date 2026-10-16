/*
 * tvgl: exeDSP's GL on Linux/Mesa EGL and GLES2.
 *
 * A Linux/Mesa GLES2 renderer, run by the shim in its own runtime
 * root so modern libraries remain separate from the TV's glibc:
 *
 *   tvgl CMD_FD REPLY_FD BUF_FD PHYS_FD (or tvgl --selftest)
 *
 * It runs the command batches the shim sends (glproto.h).  The TV's pixmap
 * surfaces are textures behind framebuffer objects: what is drawn is
 * written back to the TV's memory (PHYS_FD, the shim's sparse file at
 * offset = physical address) at sync points, and the TV memory under a
 * surface is loaded again when the GA has written to it.  GA surfaces used
 * as textures (EGLImages) are uploaded from the TV's memory, again when the
 * GA writes to them or their contents change.
 *
 * The TV's shader programs are Mali binaries: they run here as GLSL.
 */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <ucontext.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../glproto.h"

#define MAXATTR 8
#define MAXUNIT 8
#define MAXUNI 96

static int cmd_fd, rep_fd, phys_fd;
static int debug;               /* HD2GL_DEBUG: log draws and 3D uniforms */
static uint8_t *buf;

static void hlog(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    int n = snprintf(b, sizeof(b), "[hd2gl] ");
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof(b) - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 2)
        n = sizeof(b) - 2;
    b[n++] = '\n';
    ssize_t ignored = write(2, b, n);
    (void)ignored;
}

static volatile sig_atomic_t last_op, last_fn;
static void renderer_crash(int sig, siginfo_t *si, void *context)
{
    ucontext_t *uc = context;
    hlog("fatal signal %d at protocol op %d, function %d; pc=%lx lr=%lx address=%p",
         sig, last_op, last_fn, uc->uc_mcontext.arm_pc, uc->uc_mcontext.arm_lr, si->si_addr);
    int fd = open("/proc/self/maps", O_RDONLY);
    char maps[4096];
    ssize_t n;
    while (fd >= 0 && (n = read(fd, maps, sizeof(maps))) > 0) {
        ssize_t ignored = write(2, maps, n); (void)ignored;
    }
    if (fd >= 0) close(fd);
    _exit(128 + sig);
}

/* ------------------------------------------------------- the TV's memory */

struct pmap { uint8_t *base; size_t len; uint8_t *p; };

static int phys_map(uint32_t pa, uint32_t len, struct pmap *m)
{
    uint32_t a = pa & ~0xfffu;
    m->len = (pa - a) + len;
    m->base = mmap(0, m->len, PROT_READ | PROT_WRITE,
                   MAP_SHARED, phys_fd, (off_t)(uint64_t)a);
    if (m->base == MAP_FAILED || !m->base) {
        hlog("cannot map %08x+%x: %d", pa, len, errno);
        return -1;
    }
    m->p = m->base + (pa - a);
    return 0;
}

static void phys_unmap(struct pmap *m)
{
    munmap(m->base, m->len);
}

/* -------------------------------------------------------------- objects */

static GLuint tex_r[GLP_MAXOBJ], buf_r[GLP_MAXOBJ], rb_r[GLP_MAXOBJ];
static struct { uint8_t *data; uint32_t size; } bufs[GLP_MAXOBJ];   /* copies */
static GLuint fb_r[GLP_MAXCTX][GLP_MAXOBJ];     /* framebuffers are per context */

struct ptex {                   /* a texture from TV memory (EGLImage) */
    uint32_t phys, w, h, stride;
    int stale;
    uint32_t hash, epoch;
    uint32_t checked;           /* ms: contents last hashed */
};
static struct ptex ptex[GLP_MAXOBJ];

struct shader { uint32_t type, kind; };
static struct shader shaders[GLP_MAXOBJ];

struct prog {
    GLuint real;
    uint32_t vs, fs;            /* attached shaders */
    char attr[MAXATTR][24];
    char uni[MAXUNI][40];
    GLint loc[MAXUNI];          /* -2: not looked up */
};
static struct prog *progs[GLP_MAXOBJ];

struct surf {
    uint32_t w, h, stride, phys;
    GLuint tex, depth;
    GLuint fbo[GLP_MAXCTX];
    int stale;
    int dx0, dy0, dx1, dy1;     /* drawn since written back (GL rows) */
};
static struct surf *surfs[GLP_MAXOBJ];

struct ctx {
    EGLContext real;
    uint32_t surf, fb, prog, array_buf, elem_buf;
    int unit;
    uint32_t bound[MAXUNIT];
    int vp[4], sc[4], scissor;
    void *scratch[MAXATTR];
    size_t scratch_len[MAXATTR];
    struct {                    /* the arrays, for where a draw lands */
        uint32_t size, type, stride, buffer;
        const uint8_t *data;    /* vertex 0 */
    } arr[MAXATTR];
};
static struct ctx ctxs[GLP_MAXCTX];
static int cur = -1;            /* the current context */

static EGLDisplay dpy;
static EGLConfig cfg;
static EGLSurface pbuf;
static EGLContext root;
static uint32_t epoch = 1;

static GLuint real_buf(uint32_t id)
{
    if (!id || id >= GLP_MAXOBJ) return 0;
    if (!buf_r[id]) glGenBuffers(1, &buf_r[id]);
    return buf_r[id];
}

static GLuint real_tex(uint32_t id)
{
    if (!id || id >= GLP_MAXOBJ) return 0;
    if (!tex_r[id]) glGenTextures(1, &tex_r[id]);
    return tex_r[id];
}

static GLuint real_rb(uint32_t id)
{
    if (!id || id >= GLP_MAXOBJ) return 0;
    if (!rb_r[id]) glGenRenderbuffers(1, &rb_r[id]);
    return rb_r[id];
}

static GLuint real_fb(uint32_t id)
{
    if (!id || id >= GLP_MAXOBJ || cur < 0) return 0;
    if (!fb_r[cur][id]) glGenFramebuffers(1, &fb_r[cur][id]);
    return fb_r[cur][id];
}

/* --------------------------------------------------------------- shaders */

static const char vs2d[] =
    "attribute highp vec4 inVertex;\n"
    "attribute mediump vec4 inTexCoord;\n"
    "varying mediump vec2 texCoord;\n"
    "void main() {\n"
    "    gl_Position = inVertex + vec4(-1.0, 1.0, 0.0, 0.0);\n"
    "    texCoord = inTexCoord.xy;\n"
    "}\n";
static const char fs2d[] =
    "uniform sampler2D texture;\n"
    "uniform mediump float alphaConstant;\n"
    "varying mediump vec2 texCoord;\n"
    "void main() {\n"
    "    mediump vec4 t = texture2D(texture, texCoord);\n"
    "    gl_FragColor = vec4(t.rgb, t.a * alphaConstant);\n"
    "}\n";
/* the panel shader without its lights, fog, depth of field and deforms */
static const char vs3d[] =
    "attribute highp vec4 inVertex;\n"
    "attribute mediump vec4 inTexCoord;\n"
    "uniform highp mat4 mVPMatrix;\n"
    "uniform highp mat4 mModel;\n"
    "uniform mediump float mAlpha;\n"
    "varying mediump vec2 TexCoord;\n"
    "varying mediump vec4 LocalColor;\n"
    "void main() {\n"
    "    gl_Position = mVPMatrix * (mModel * inVertex);\n"
    "    TexCoord = inTexCoord.xy;\n"
    "    LocalColor = vec4(1.0, 1.0, 1.0, mAlpha);\n"
    "}\n";
static const char fs3d[] =
    "uniform sampler2D texture;\n"
    "varying mediump vec2 TexCoord;\n"
    "varying mediump vec4 LocalColor;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(texture, TexCoord) * LocalColor;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    char log[512];

    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, sizeof(log), 0, log);
        hlog("shader: %s", log);
    }
    return s;
}

static void link_program(uint32_t id)
{
    struct prog *p = progs[id];
    bool is3d;
    GLint ok = 0;
    char log[512];

    if (!p)
        return;
    is3d = shaders[p->vs].kind == KIND_3D_VERT;
    if (shaders[p->vs].kind == KIND_UNKNOWN)
        hlog("program %u: unknown shaders, drawn as 2D", id);
    if (p->real)
        glDeleteProgram(p->real);
    p->real = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, is3d ? vs3d : vs2d);
    GLuint fs = compile(GL_FRAGMENT_SHADER, is3d ? fs3d : fs2d);
    glAttachShader(p->real, vs);
    glAttachShader(p->real, fs);
    /* The program retains these until it is deleted. */
    glDeleteShader(vs);
    glDeleteShader(fs);
    for (int i = 0; i < MAXATTR; i++)
        if (p->attr[i][0])
            glBindAttribLocation(p->real, i, p->attr[i]);
    glLinkProgram(p->real);
    glGetProgramiv(p->real, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p->real, sizeof(log), 0, log);
        hlog("program %u: %s", id, log);
    }
    for (int i = 0; i < MAXUNI; i++)
        p->loc[i] = -2;
    hlog("program %u: %s", id, is3d ? "3D panels" : "2D");
}

/* ---------------------------------------------------- surfaces, textures */

static void bind_fb(void)
{
    struct ctx *c = &ctxs[cur];

    if (c->fb) {
        glBindFramebuffer(GL_FRAMEBUFFER, real_fb(c->fb));
        return;
    }
    struct surf *s = c->surf ? surfs[c->surf] : 0;
    if (!s) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    if (!s->fbo[cur]) {
        glGenFramebuffers(1, &s->fbo[cur]);
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[cur]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s->depth);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE)
            hlog("surface fbo: status %x", st);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[cur]);
}

/* a scratch texture unit for uploads: binds tex there, restores after */
static void with_tex(GLuint tex)
{
    glActiveTexture(GL_TEXTURE0 + MAXUNIT - 1);
    glBindTexture(GL_TEXTURE_2D, tex);
}

static void restore_tex(void)
{
    struct ctx *c = &ctxs[cur];
    glBindTexture(GL_TEXTURE_2D, real_tex(c->bound[MAXUNIT - 1]));
    glActiveTexture(GL_TEXTURE0 + c->unit);
}

/* TV memory rows (top first) into a texture (GL rows, bottom first when
 * flip) */
static void load(GLuint tex, uint32_t phys, uint32_t w, uint32_t h, uint32_t stride,
                 int flip, int alloc)
{
    struct pmap m;
    uint32_t *tmp = 0;

    if (phys_map(phys, stride * h, &m))
        return;
    if (flip || stride != w * 4) {
        tmp = malloc((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; y++)
            memcpy(tmp + (size_t)y * w, m.p + (size_t)(flip ? h - 1 - y : y) * stride, w * 4);
    }
    with_tex(tex);
    if (alloc)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     tmp ? (void *)tmp : (void *)m.p);
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                        tmp ? (void *)tmp : (void *)m.p);
    restore_tex();
    free(tmp);
    phys_unmap(&m);
}

static uint32_t ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t hash_phys(struct ptex *t)
{
    struct pmap m;
    uint32_t h = 2166136261u;

    if (phys_map(t->phys, t->stride * t->h, &m))
        return 0;
    for (uint32_t y = 0; y < t->h; y += 4) {
        const uint32_t *row = (const uint32_t *)(m.p + (size_t)y * t->stride);
        for (uint32_t x = 0; x < t->w; x++)
            h = (h ^ row[x]) * 16777619u;
    }
    phys_unmap(&m);
    return h;
}

/* before a draw: the textures it may sample and the target up to date */
static void prepare_draw(void)
{
    struct ctx *c = &ctxs[cur];

    for (int u = 0; u < MAXUNIT - 1; u++) {
        uint32_t id = c->bound[u];
        struct ptex *t = id ? &ptex[id] : 0;
        if (!t || !t->phys)
            continue;
        /* CPU writes (not reported like the GA's): looked for once a second */
        if (t->epoch != epoch && ms() - t->checked >= 1000) {
            uint32_t h = hash_phys(t);
            t->epoch = epoch;
            t->checked = ms();
            if (h != t->hash) {
                t->hash = h;
                t->stale = 1;
            }
        }
        if (t->stale) {
            load(real_tex(id), t->phys, t->w, t->h, t->stride, 0, 0);
            t->stale = 0;
        }
    }
    if (!c->fb && c->surf && surfs[c->surf]) {
        struct surf *s = surfs[c->surf];
        if (s->stale) {
            load(s->tex, s->phys, s->w, s->h, s->stride, 1, 0);
            s->stale = 0;
        }
    }
}

/* the target's area a draw or clear may change */
/* the target's area a draw or clear may change: the whole surface, the
 * viewport, or (rect) a window rectangle */
static void mark_drawn_rect(int whole, const int *rect)
{
    struct ctx *c = &ctxs[cur];
    struct surf *s;
    int x0, y0, x1, y1;

    if (c->fb || !c->surf || !(s = surfs[c->surf]))
        return;
    if (whole) {
        x0 = 0; y0 = 0; x1 = s->w; y1 = s->h;
    } else if (rect) {
        x0 = rect[0]; y0 = rect[1]; x1 = rect[2]; y1 = rect[3];
    } else {
        x0 = c->vp[0]; y0 = c->vp[1]; x1 = c->vp[0] + c->vp[2]; y1 = c->vp[1] + c->vp[3];
    }
    if (c->scissor) {
        if (c->sc[0] > x0) x0 = c->sc[0];
        if (c->sc[1] > y0) y0 = c->sc[1];
        if (c->sc[0] + c->sc[2] < x1) x1 = c->sc[0] + c->sc[2];
        if (c->sc[1] + c->sc[3] < y1) y1 = c->sc[1] + c->sc[3];
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)s->w) x1 = s->w;
    if (y1 > (int)s->h) y1 = s->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (s->dx1 <= s->dx0) {
        s->dx0 = x0; s->dy0 = y0; s->dx1 = x1; s->dy1 = y1;
    } else {
        if (x0 < s->dx0) s->dx0 = x0;
        if (y0 < s->dy0) s->dy0 = y0;
        if (x1 > s->dx1) s->dx1 = x1;
        if (y1 > s->dy1) s->dy1 = y1;
    }
}

static void mark_drawn(int whole)
{
    mark_drawn_rect(whole, 0);
}

/* where a 2D draw of vertices [first, first + n) lands: the 2D program puts
 * inVertex + (-1, 1) in clip space */
static int draw_rect(uint32_t first, uint32_t n, int *r)
{
    struct ctx *c = &ctxs[cur];
    struct prog *p = c->prog ? progs[c->prog] : 0;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int a = -1;

    if (!p || shaders[p->vs].kind == KIND_3D_VERT)
        return 0;
    for (int i = 0; i < MAXATTR; i++)
        if (!strcmp(p->attr[i], "inVertex"))
            a = i;
    if (a < 0 || c->arr[a].type != GL_FLOAT || c->arr[a].size < 2 || !c->arr[a].data)
        return 0;
    for (uint32_t v = first; v < first + n; v++) {
        const float *f = (const float *)(c->arr[a].data + (size_t)v * c->arr[a].stride);
        float x = f[0] - 1, y = f[1] + 1;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    r[0] = (int)(c->vp[0] + (x0 + 1) * 0.5f * c->vp[2]) - 1;
    r[1] = (int)(c->vp[1] + (y0 + 1) * 0.5f * c->vp[3]) - 1;
    r[2] = (int)(c->vp[0] + (x1 + 1) * 0.5f * c->vp[2]) + 2;
    r[3] = (int)(c->vp[1] + (y1 + 1) * 0.5f * c->vp[3]) + 2;
    if (r[0] < c->vp[0]) r[0] = c->vp[0];
    if (r[1] < c->vp[1]) r[1] = c->vp[1];
    if (r[2] > c->vp[0] + c->vp[2]) r[2] = c->vp[0] + c->vp[2];
    if (r[3] > c->vp[1] + c->vp[3]) r[3] = c->vp[1] + c->vp[3];
    return 1;
}

/* drawn surfaces back to TV memory */
static void write_back(void)
{
    int made = 0;

    for (int i = 1; i < GLP_MAXOBJ; i++) {
        struct surf *s = surfs[i];
        if (!s || s->dx1 <= s->dx0)
            continue;
        if (cur < 0) {
            eglMakeCurrent(dpy, pbuf, pbuf, root);
            made = 1;
        }
        int ctx = cur < 0 ? 0 : cur;
        GLuint fbo;
        if (!s->fbo[ctx]) {
            glGenFramebuffers(1, &s->fbo[ctx]);
            glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[ctx]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s->depth);
        }
        fbo = s->fbo[ctx];
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        int w = s->dx1 - s->dx0, h = s->dy1 - s->dy0;
        uint32_t *tmp = malloc((size_t)w * h * 4);
        if (debug) hlog("readback surface %d: rect %d,%d %dx%d, surface %ux%u stride %u, buffer %p",
                         i, s->dx0, s->dy0, w, h, s->w, s->h, s->stride, (void *)tmp);
        glReadPixels(s->dx0, s->dy0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        struct pmap m;
        if (!phys_map(s->phys, s->stride * s->h, &m)) {
            for (int j = 0; j < h; j++)
                memcpy(m.p + (size_t)(s->h - 1 - (s->dy0 + j)) * s->stride + s->dx0 * 4,
                       tmp + (size_t)j * w, w * 4);
            phys_unmap(&m);
        }
        free(tmp);
        s->dx0 = s->dy0 = s->dx1 = s->dy1 = 0;
        if (cur >= 0)
            bind_fb();
    }
    if (made)
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

/* GA writes: surfaces and textures over them load again */
static void phys_dirty(uint32_t pa, uint32_t len)
{
    for (int i = 1; i < GLP_MAXOBJ; i++) {
        struct surf *s = surfs[i];
        if (s && pa < s->phys + s->stride * s->h && s->phys < pa + len)
            s->stale = 1;
        struct ptex *t = &ptex[i];
        if (t->phys && pa < t->phys + t->stride * t->h && t->phys < pa + len)
            t->stale = 1;
    }
}

/* ------------------------------------------------------------ the calls */

static float fl(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static void gl_call(const uint32_t *a)
{
    struct ctx *c = &ctxs[cur];

    switch (a[0]) {
    case F_ClearColor: glClearColor(fl(a[3]), fl(a[2]), fl(a[1]), fl(a[4])); break;   /* B, G, R */
    case F_ClearDepthf: glClearDepthf(fl(a[1])); break;
    case F_Clear: prepare_draw(); glClear(a[1]); mark_drawn(1); break;
    case F_Viewport:
        for (int i = 0; i < 4; i++) c->vp[i] = a[1 + i];
        glViewport(a[1], a[2], a[3], a[4]);
        break;
    case F_Scissor:
        for (int i = 0; i < 4; i++) c->sc[i] = a[1 + i];
        glScissor(a[1], a[2], a[3], a[4]);
        break;
    case F_Enable: if (a[1] == GL_SCISSOR_TEST) c->scissor = 1; glEnable(a[1]); break;
    case F_Disable: if (a[1] == GL_SCISSOR_TEST) c->scissor = 0; glDisable(a[1]); break;
    case F_BlendFunc: glBlendFunc(a[1], a[2]); break;
    case F_BlendFuncSeparate: glBlendFuncSeparate(a[1], a[2], a[3], a[4]); break;
    case F_BlendEquation: glBlendEquation(a[1]); break;
    case F_ColorMask: glColorMask(a[3], a[2], a[1], a[4]); break;     /* B, G, R */
    case F_DepthMask: glDepthMask(a[1]); break;
    case F_DepthFunc: glDepthFunc(a[1]); break;
    case F_CullFace: glCullFace(a[1]); break;
    case F_FrontFace: glFrontFace(a[1]); break;
    case F_BindBuffer:
        if (a[1] == GL_ARRAY_BUFFER) c->array_buf = a[2];
        else c->elem_buf = a[2];
        glBindBuffer(a[1], real_buf(a[2]));
        break;
    case F_DeleteBuffer:
        if (a[1] < GLP_MAXOBJ && buf_r[a[1]]) {
            glDeleteBuffers(1, &buf_r[a[1]]);
            buf_r[a[1]] = 0;
        }
        if (a[1] < GLP_MAXOBJ) {
            free(bufs[a[1]].data);
            bufs[a[1]].data = 0;
            bufs[a[1]].size = 0;
        }
        break;
    case F_ActiveTexture: c->unit = (a[1] - GL_TEXTURE0) & (MAXUNIT - 1); glActiveTexture(a[1]); break;
    case F_BindTexture:
        c->bound[c->unit] = a[2] < GLP_MAXOBJ ? a[2] : 0;
        glBindTexture(a[1], real_tex(a[2]));
        break;
    case F_DeleteTexture:
        if (a[1] < GLP_MAXOBJ) {
            if (tex_r[a[1]])
                glDeleteTextures(1, &tex_r[a[1]]);
            tex_r[a[1]] = 0;
            memset(&ptex[a[1]], 0, sizeof(ptex[0]));
        }
        break;
    case F_TexParameteri: glTexParameteri(a[1], a[2], a[3]); break;
    case F_TexParameterf: glTexParameterf(a[1], a[2], fl(a[3])); break;
    case F_GenerateMipmap: glGenerateMipmap(a[1]); break;
    case F_BindFramebuffer: c->fb = a[2]; bind_fb(); break;
    case F_DeleteFramebuffer:
        if (a[1] < GLP_MAXOBJ && fb_r[cur][a[1]]) {
            glDeleteFramebuffers(1, &fb_r[cur][a[1]]);
            fb_r[cur][a[1]] = 0;
        }
        break;
    case F_FramebufferTexture2D: glFramebufferTexture2D(a[1], a[2], a[3], real_tex(a[4]), a[5]); break;
    case F_FramebufferRenderbuffer: glFramebufferRenderbuffer(a[1], a[2], a[3], real_rb(a[4])); break;
    case F_BindRenderbuffer: glBindRenderbuffer(a[1], real_rb(a[2])); break;
    case F_DeleteRenderbuffer:
        if (a[1] < GLP_MAXOBJ && rb_r[a[1]]) {
            glDeleteRenderbuffers(1, &rb_r[a[1]]);
            rb_r[a[1]] = 0;
        }
        break;
    case F_RenderbufferStorage: {
        GLenum f = a[2];
        if (f == GL_RGBA || f == 0x8058)                /* RGBA8_OES */
            f = GL_RGBA4;
        if (f == 0x81A6)                                /* DEPTH24: 16 here */
            f = GL_DEPTH_COMPONENT16;
        glRenderbufferStorage(a[1], f, a[3], a[4]);
        break;
    }
    case F_CreateProgram:
        if (a[1] < GLP_MAXOBJ) {
            if (progs[a[1]] && progs[a[1]]->real)
                glDeleteProgram(progs[a[1]]->real);
            free(progs[a[1]]);
            progs[a[1]] = calloc(1, sizeof(struct prog));
        }
        break;
    case F_DeleteProgram:
        if (a[1] < GLP_MAXOBJ && progs[a[1]]) {
            if (progs[a[1]]->real)
                glDeleteProgram(progs[a[1]]->real);
            free(progs[a[1]]);
            progs[a[1]] = 0;
        }
        break;
    case F_AttachShader:
        if (a[1] < GLP_MAXOBJ && progs[a[1]] && a[2] < GLP_MAXOBJ) {
            if (shaders[a[2]].type == GL_FRAGMENT_SHADER) progs[a[1]]->fs = a[2];
            else progs[a[1]]->vs = a[2];
        }
        break;
    case F_LinkProgram: if (a[1] < GLP_MAXOBJ) link_program(a[1]); break;
    case F_UseProgram:
        c->prog = a[1] < GLP_MAXOBJ ? a[1] : 0;
        glUseProgram(c->prog && progs[c->prog] ? progs[c->prog]->real : 0);
        break;
    case F_DeleteShader: break;
    case F_EnableVertexAttribArray: glEnableVertexAttribArray(a[1]); break;
    case F_DisableVertexAttribArray: glDisableVertexAttribArray(a[1]); break;
    case F_VertexAttrib4f: glVertexAttrib4f(a[1], fl(a[2]), fl(a[3]), fl(a[4]), fl(a[5])); break;
    case F_Hint: glHint(a[1], a[2]); break;
    case F_LineWidth: glLineWidth(fl(a[1])); break;
    case F_PolygonOffset: glPolygonOffset(fl(a[1]), fl(a[2])); break;
    case F_Finish: glFinish(); break;
    default: hlog("call %u?", a[0]);
    }
}

static void uniform(const uint32_t *a)
{
    struct ctx *c = &ctxs[cur];
    struct prog *p = c->prog ? progs[c->prog] : 0;
    int l = a[0], kind = a[1], n = a[2], count = a[3];
    const void *v = a + 4;

    if (!p || !p->real || l < 0 || l >= MAXUNI || !p->uni[l][0])
        return;
    if (p->loc[l] == -2)
        p->loc[l] = glGetUniformLocation(p->real, p->uni[l]);
    GLint loc = p->loc[l];
    if (debug) {
        const float *f = v;
        hlog("uniform %s (%d x %d): %g %g %g %g", p->uni[l], n, count,
             kind == 1 ? (float)((const int *)v)[0] : f[0], n > 1 ? f[1] : 0,
             n > 2 ? f[2] : 0, n > 3 ? f[3] : 0);
    }
    if (loc < 0)
        return;
    switch (kind) {
    case 0:
        if (n == 1) glUniform1fv(loc, count, v);
        else if (n == 2) glUniform2fv(loc, count, v);
        else if (n == 3) glUniform3fv(loc, count, v);
        else glUniform4fv(loc, count, v);
        break;
    case 1:
        if (n == 1) glUniform1iv(loc, count, v);
        else if (n == 2) glUniform2iv(loc, count, v);
        else if (n == 3) glUniform3iv(loc, count, v);
        else glUniform4iv(loc, count, v);
        break;
    case 2: glUniformMatrix2fv(loc, count, GL_FALSE, v); break;
    case 3: glUniformMatrix3fv(loc, count, GL_FALSE, v); break;
    case 4: glUniformMatrix4fv(loc, count, GL_FALSE, v); break;
    }
}

static void attrib(const uint32_t *a)
{
    struct ctx *c = &ctxs[cur];
    uint32_t i = a[0];

    if (i >= MAXATTR)
        return;
    if (a[5] == GLP_INLINE) {
        uint32_t bytes = a[8];
        if (c->scratch_len[i] < bytes) {
            free(c->scratch[i]);
            c->scratch[i] = malloc(bytes);
            c->scratch_len[i] = bytes;
        }
        memcpy(c->scratch[i], a + 9, bytes);
        c->arr[i].data = (uint8_t *)c->scratch[i] - (size_t)a[7] * a[4];
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(i, a[1], a[2], a[3], a[4],
                              (uint8_t *)c->scratch[i] - (size_t)a[7] * a[4]);
    } else {
        c->arr[i].data = a[5] < GLP_MAXOBJ && bufs[a[5]].data && a[6] < bufs[a[5]].size ?
                         bufs[a[5]].data + a[6] : 0;
        glBindBuffer(GL_ARRAY_BUFFER, real_buf(a[5]));
        glVertexAttribPointer(i, a[1], a[2], a[3], a[4], (void *)(uintptr_t)a[6]);
    }
    c->arr[i].size = a[1];
    c->arr[i].type = a[2];
    c->arr[i].stride = a[4];
    glBindBuffer(GL_ARRAY_BUFFER, real_buf(c->array_buf));
}

/* FBO names belong to their creating context; textures/depth are shared. */
static void destroy_surface(uint32_t id)
{
    struct surf *s = id < GLP_MAXOBJ ? surfs[id] : 0;
    if (!s) return;
    EGLContext saved = eglGetCurrentContext();
    for (int i = 0; i < GLP_MAXCTX; i++) {
        if (s->fbo[i] && ctxs[i].real &&
            eglMakeCurrent(dpy, pbuf, pbuf, ctxs[i].real))
            glDeleteFramebuffers(1, &s->fbo[i]);
        if (ctxs[i].surf == id) ctxs[i].surf = 0;
    }
    if (eglMakeCurrent(dpy, pbuf, pbuf, root)) {
        glDeleteTextures(1, &s->tex);
        glDeleteRenderbuffers(1, &s->depth);
    }
    free(s);
    surfs[id] = 0;
    eglMakeCurrent(dpy, saved ? pbuf : EGL_NO_SURFACE,
                   saved ? pbuf : EGL_NO_SURFACE, saved);
    if (cur >= 0) bind_fb();
}

static void destroy_context(uint32_t id)
{
    if (!id || id >= GLP_MAXCTX || !ctxs[id].real) return;
    if (cur == (int)id) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        cur = -1;
    }
    eglDestroyContext(dpy, ctxs[id].real);
    for (int i = 0; i < MAXATTR; i++) free(ctxs[id].scratch[i]);
    memset(&ctxs[id], 0, sizeof(ctxs[id]));
    memset(fb_r[id], 0, sizeof(fb_r[id]));
    for (int i = 0; i < GLP_MAXOBJ; i++)
        if (surfs[i]) surfs[i]->fbo[id] = 0;
}

static void memory_stats(void)
{
    static time_t last;
    time_t now = time(0);
    if (now - last < 10) return;
    last = now;
    unsigned ns = 0, nt = 0, nc = 0;
    for (int i = 0; i < GLP_MAXOBJ; i++) {
        ns += surfs[i] != 0; nt += tex_r[i] != 0;
    }
    for (int i = 0; i < GLP_MAXCTX; i++) nc += ctxs[i].real != 0;
    struct mallinfo2 m = mallinfo2();
    hlog("memory: heap-used=%zuKiB heap-free=%zuKiB mmap=%zuKiB surfaces=%u textures=%u contexts=%u",
         m.uordblks / 1024, m.fordblks / 1024, m.hblkhd / 1024, ns, nt, nc);
}

static void run(uint32_t n)
{
    uint32_t off = 0;
    uint8_t *reply = buf + GLP_CMD_SIZE;

    while (off + 8 <= n) {
        const uint32_t *h = (const uint32_t *)(buf + off);
        const uint32_t *a = h + 2;
        uint32_t op = h[0];

        if (h[1] > (n - off - 8) / 4) {
            hlog("invalid command length at %u", off);
            return;
        }
        last_op = op;
        last_fn = op == GLP_CALL && h[1] ? a[0] : 0;
        off += 8 + 4 * h[1];
        if (cur < 0 && op >= GLP_CALL) {
            continue;                   /* GL without a context */
        }
        switch (op) {
        case GLP_CTX_CREATE: {
            static const EGLint attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
            if (a[0] < GLP_MAXCTX) {
                destroy_context(a[0]);
                memset(&ctxs[a[0]], 0, sizeof(ctxs[0]));
                ctxs[a[0]].real = eglCreateContext(dpy, cfg, root, attr);
                if (!ctxs[a[0]].real)
                    hlog("eglCreateContext: %x", eglGetError());
            }
            break;
        }
        case GLP_CTX_DESTROY:
            destroy_context(a[0]);
            break;
        case GLP_SURF_PIXMAP: {
            struct surf *s;
            int made = cur < 0;
            if (a[0] >= GLP_MAXOBJ)
                break;
            destroy_surface(a[0]);
            if (made)
                eglMakeCurrent(dpy, pbuf, pbuf, root);
            s = calloc(1, sizeof(*s));
            s->w = a[1]; s->h = a[2]; s->stride = a[3]; s->phys = a[4];
            glGenTextures(1, &s->tex);
            glGenRenderbuffers(1, &s->depth);
            glBindRenderbuffer(GL_RENDERBUFFER, s->depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, s->w, s->h);
            if (made) {
                cur = 0;                /* for with_tex/restore_tex */
                memset(&ctxs[0], 0, sizeof(ctxs[0]));
                ctxs[0].real = root;
            }
            load(s->tex, s->phys, s->w, s->h, s->stride, 1, 1);
            with_tex(s->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            restore_tex();
            surfs[a[0]] = s;
            if (made) {
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                cur = -1;
            }
            hlog("surface %u: %ux%u at %08x", a[0], s->w, s->h, s->phys);
            break;
        }
        case GLP_SURF_DESTROY:
            destroy_surface(a[0]);
            break;
        case GLP_MAKE_CURRENT:
            if (!a[0] || a[0] >= GLP_MAXCTX || !ctxs[a[0]].real) {
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                cur = -1;
                break;
            }
            if (!eglMakeCurrent(dpy, pbuf, pbuf, ctxs[a[0]].real))
                hlog("eglMakeCurrent: %x", eglGetError());
            cur = a[0];
            ctxs[cur].surf = a[1] < GLP_MAXOBJ ? a[1] : 0;
            bind_fb();
            break;
        case GLP_TEX_PHYS:
            if (a[0] && a[0] < GLP_MAXOBJ && cur >= 0) {
                struct ptex *t = &ptex[a[0]];
                t->w = a[1]; t->h = a[2]; t->stride = a[3]; t->phys = a[4];
                t->epoch = epoch;
                t->hash = hash_phys(t);
                load(real_tex(a[0]), t->phys, t->w, t->h, t->stride, 0, 1);
                t->stale = 0;
            }
            break;
        case GLP_PHYS_DIRTY: phys_dirty(a[0], a[1]); break;
        case GLP_SYNC:
            write_back();
            epoch++;
            break;
        case GLP_CALL: gl_call(a); break;
        case GLP_BUFFER_DATA: {
            uint32_t id = a[0] == GL_ARRAY_BUFFER ? ctxs[cur].array_buf : ctxs[cur].elem_buf;
            if (id && id < GLP_MAXOBJ) {
                free(bufs[id].data);
                bufs[id].data = malloc(a[1] ? a[1] : 1);
                memcpy(bufs[id].data, a + 3, a[1]);
                bufs[id].size = a[1];
            }
            glBufferData(a[0], a[1], a + 3, a[2]);
            break;
        }
        case GLP_BUFFER_SUBDATA: {
            uint32_t id = a[0] == GL_ARRAY_BUFFER ? ctxs[cur].array_buf : ctxs[cur].elem_buf;
            if (id && id < GLP_MAXOBJ && bufs[id].data && a[1] + a[2] <= bufs[id].size)
                memcpy(bufs[id].data + a[1], a + 3, a[2]);
            glBufferSubData(a[0], a[1], a[2], a + 3);
            break;
        }
        case GLP_TEX_IMAGE:
            glTexImage2D(GL_TEXTURE_2D, a[0], GL_RGBA, a[1], a[2], 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            break;
        case GLP_TEX_SUBIMAGE:
            glTexSubImage2D(GL_TEXTURE_2D, a[0], a[1], a[2], a[3], a[4], GL_RGBA,
                            GL_UNSIGNED_BYTE, a + 5);
            break;
        case GLP_SHADER:
            if (a[0] < GLP_MAXOBJ) {
                shaders[a[0]].type = a[1];
                shaders[a[0]].kind = a[2];
            }
            break;
        case GLP_BIND_ATTRIB:
            if (a[0] < GLP_MAXOBJ && progs[a[0]] && a[1] < MAXATTR)
                snprintf(progs[a[0]]->attr[a[1]], 24, "%s", (const char *)(a + 2));
            break;
        case GLP_UNIFORM_NAME:
            if (a[0] < GLP_MAXOBJ && progs[a[0]] && a[1] < MAXUNI) {
                snprintf(progs[a[0]]->uni[a[1]], 40, "%s", (const char *)(a + 2));
                progs[a[0]]->loc[a[1]] = -2;
            }
            break;
        case GLP_UNIFORM: uniform(a); break;
        case GLP_ATTRIB: attrib(a); break;
        case GLP_DRAW_ARRAYS:
            if (debug) {
                struct ctx *c = &ctxs[cur];
                struct prog *p = c->prog ? progs[c->prog] : 0;
                hlog("draw %s mode %u n %u vp %d,%d %dx%d scissor %d fb %u surf %u tex %u (%08x)",
                     p && shaders[p->vs].kind == KIND_3D_VERT ? "3D" : "2D", a[0], a[2],
                     c->vp[0], c->vp[1], c->vp[2], c->vp[3], c->scissor, c->fb, c->surf,
                     c->bound[0], ptex[c->bound[0]].phys);
            }
            prepare_draw();
            glDrawArrays(a[0], a[1], a[2]);
            {
                int r[4];
                mark_drawn_rect(0, draw_rect(a[1], a[2], r) ? r : 0);
            }
            break;
        case GLP_DRAW_ELEMENTS:
            prepare_draw();
            if (a[3] == GLP_INLINE) {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glDrawElements(a[0], a[1], a[2], a + 5);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, real_buf(ctxs[cur].elem_buf));
            } else {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, real_buf(a[3]));
                glDrawElements(a[0], a[1], a[2], (void *)(uintptr_t)a[4]);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, real_buf(ctxs[cur].elem_buf));
            }
            mark_drawn(0);
            break;
        case GLP_READ_PIXELS:
            glReadPixels(a[0], a[1], a[2], a[3], GL_RGBA, GL_UNSIGNED_BYTE, reply);
            break;
        default:
            hlog("op %u?", op);
        }
    }
}

int main(int argc, char **argv)
{
    static const EGLint ca[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    static const EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLint n, maj, min;
    uint32_t len, r;

    struct sigaction crash_action = {0};
    crash_action.sa_sigaction = renderer_crash;
    crash_action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&crash_action.sa_mask);
    sigaction(SIGSEGV, &crash_action, 0);
    sigaction(SIGBUS, &crash_action, 0);
    bool selftest = argc == 2 && !strcmp(argv[1], "--selftest");
    if (!selftest && argc != 5) {
        hlog("usage: tvgl CMD_FD REPLY_FD BUF_FD PHYS_FD | --selftest");
        return 1;
    }
    debug = getenv("HD2GL_DEBUG") && strcmp(getenv("HD2GL_DEBUG"), "0");
    if (!selftest) {
    cmd_fd = atoi(argv[1]);
    rep_fd = atoi(argv[2]);
    phys_fd = atoi(argv[4]);
    buf = mmap(0, GLP_CMD_SIZE + GLP_REPLY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
               atoi(argv[3]), 0);
    if (buf == MAP_FAILED) {
        hlog("no command buffer");
        return 1;
    }
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        hlog("cannot select GLES API: %x", eglGetError());
        return 1;
    }
    /* Softpipe is the conservative ARM32 baseline. llvmpipe/virgl remain
     * selectable through GALLIUM_DRIVER and LIBGL_ALWAYS_SOFTWARE. */
    const char *software = getenv("LIBGL_ALWAYS_SOFTWARE");
    if (!getenv("GALLIUM_DRIVER") && software && strcmp(software, "0"))
        setenv("GALLIUM_DRIVER", "softpipe", 0);
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, &maj, &min) || !eglChooseConfig(dpy, ca, &cfg, 1, &n) || !n ||
        !(pbuf = eglCreatePbufferSurface(dpy, cfg, pa)) ||
        !(root = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa)) ||
        !eglMakeCurrent(dpy, pbuf, pbuf, root)) {
        hlog("no EGL: %x", eglGetError());
        return 1;
    }
    hlog("%s, %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (selftest) {
        const char *vs[] = { vs2d, vs3d }, *fs[] = { fs2d, fs3d };
        for (int i = 0; i < 2; i++) {
            GLuint v = compile(GL_VERTEX_SHADER, vs[i]);
            GLuint f = compile(GL_FRAGMENT_SHADER, fs[i]);
            GLint ok = 0;
            glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
            if (!ok) return 2;
            glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
            if (!ok) return 2;
            GLuint p = glCreateProgram();
            glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
            glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok) { hlog("selftest: shader link failed"); return 2; }
            if (i == 0) {
                const GLfloat vertices[] = {0,-2,0,1, 2,-2,0,1, 0,0,0,1, 2,0,0,1};
                const GLfloat uv[] = {0,0, 1,0, 0,1, 1,1};
                const GLubyte texel[] = {64,128,191,255};
                GLubyte pixel[4] = {0};
                GLuint tex;
                glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glUseProgram(p);
                glUniform1i(glGetUniformLocation(p, "texture"), 0);
                glUniform1f(glGetUniformLocation(p, "alphaConstant"), 1.0f);
                GLint vertex = glGetAttribLocation(p, "inVertex");
                GLint coord = glGetAttribLocation(p, "inTexCoord");
                glEnableVertexAttribArray(vertex); glEnableVertexAttribArray(coord);
                glVertexAttribPointer(vertex, 4, GL_FLOAT, GL_FALSE, 0, vertices);
                glVertexAttribPointer(coord, 2, GL_FLOAT, GL_FALSE, 0, uv);
                glViewport(0, 0, 16, 16);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                if (glGetError() || memcmp(pixel, texel, 4)) {
                    hlog("selftest: textured draw mismatch %u %u %u %u", pixel[0], pixel[1], pixel[2], pixel[3]);
                    return 2;
                }
                glDisableVertexAttribArray(vertex); glDisableVertexAttribArray(coord);
                glUseProgram(0); glDeleteTextures(1, &tex);
            }
            glDeleteProgram(p); glDeleteShader(v); glDeleteShader(f);
        }
        uint8_t pixel[4] = {0};
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        if (glGetError() || abs(pixel[0] - 64) > 1 || abs(pixel[1] - 128) > 1 ||
            abs(pixel[2] - 191) > 1 || pixel[3] != 255) {
            hlog("selftest: readback mismatch %u %u %u %u", pixel[0], pixel[1], pixel[2], pixel[3]);
            return 2;
        }
        hlog("selftest passed: GLES2, both shader pairs, textured draw and RGBA readback");
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, root); eglDestroySurface(dpy, pbuf); eglTerminate(dpy);
        return 0;
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    for (;;) {
        if (read(cmd_fd, &len, 4) != 4)
            return 0;
        if (len > GLP_CMD_SIZE)
            len = 0;
        run(len);
        memory_stats();
        r = 0;
        if (write(rep_fd, &r, 4) != 4)
            return 0;
    }
}
