/*
 * The TV's GL on the HD2's Adreno 200: the protocol between the shim's
 * client (gl.c, in exeDSP) and hd2gl (an Android process on the ROM's
 * EGL/GLES2, in the /hd2/android chroot).
 *
 * The client appends commands to a shared buffer and, at a sync point,
 * writes the number of bytes to the command pipe; hd2gl runs them all and
 * answers on the reply pipe with one word, any data in the reply area.
 * A command is { u32 op; u32 words; u32 arg[words] } (data padded to 4).
 *
 * Object names are the client's (it allocates them); hd2gl maps them to
 * real ones.  Colours are kept as B, G, R, A in GL's R, G, B, A channels
 * (the TV's ARGB8888 words in memory), so textures from TV memory upload
 * as they are and render targets read back as they are.
 */
#pragma once

#define GLP_BUF         "/hd2/glbuf"
#define GLP_CMD_SIZE    (24u << 20)     /* the command buffer */
#define GLP_REPLY_SIZE  (8u << 20)      /* the reply area, after it */
#define GLP_MAXOBJ      4096
#define GLP_MAXCTX      16

enum glp_op {
    /* EGL and the TV's memory */
    GLP_CTX_CREATE = 1,     /* ctx */
    GLP_CTX_DESTROY,        /* ctx */
    GLP_SURF_PIXMAP,        /* surf, w, h, stride (bytes), phys (TV memory) */
    GLP_SURF_DESTROY,       /* surf */
    GLP_MAKE_CURRENT,       /* ctx, surf (0: none) */
    GLP_TEX_PHYS,           /* tex, w, h, stride (bytes), phys: an EGLImage */
    GLP_PHYS_DIRTY,         /* phys, len: written by the GA */
    GLP_SYNC,               /* write drawn surfaces back to TV memory */

    /* GL with scalar arguments: fn, args (floats as their bits) */
    GLP_CALL,

    /* GL with data */
    GLP_BUFFER_DATA,        /* target, size, usage, data */
    GLP_BUFFER_SUBDATA,     /* target, offset, size, data */
    GLP_TEX_IMAGE,          /* level, w, h, data (BGRA words, w*h) or none */
    GLP_TEX_SUBIMAGE,       /* level, x, y, w, h, data (BGRA words) */
    GLP_SHADER,             /* shader, type, kind */
    GLP_BIND_ATTRIB,        /* program, index, name */
    GLP_UNIFORM_NAME,       /* program, location, name */
    GLP_UNIFORM,            /* location, kind (0 f, 1 i, 2 mat2, 3 mat3, 4 mat4), n, count, values */
    GLP_ATTRIB,             /* index, size, type, norm, stride, buffer, offset | client data */
    GLP_DRAW_ARRAYS,        /* mode, first, count */
    GLP_DRAW_ELEMENTS,      /* mode, count, type, buffer, offset | indices */
    GLP_READ_PIXELS,        /* x, y, w, h -> BGRA words in the reply area */
};

/* GLP_CALL functions */
enum glp_fn {
    F_ClearColor = 1, F_ClearDepthf, F_Clear, F_Viewport, F_Scissor, F_Enable,
    F_Disable, F_BlendFunc, F_BlendFuncSeparate, F_BlendEquation, F_ColorMask,
    F_DepthMask, F_DepthFunc, F_CullFace, F_FrontFace, F_BindBuffer,
    F_DeleteBuffer, F_ActiveTexture, F_BindTexture, F_DeleteTexture,
    F_TexParameteri, F_TexParameterf, F_BindFramebuffer, F_DeleteFramebuffer,
    F_FramebufferTexture2D, F_FramebufferRenderbuffer, F_BindRenderbuffer,
    F_DeleteRenderbuffer, F_RenderbufferStorage, F_CreateProgram,
    F_DeleteProgram, F_AttachShader, F_LinkProgram, F_UseProgram,
    F_DeleteShader, F_EnableVertexAttribArray, F_DisableVertexAttribArray,
    F_VertexAttrib4f, F_GenerateMipmap, F_Hint, F_PixelStorei, F_LineWidth,
    F_PolygonOffset, F_Finish,
};

/* shader kinds: the TV's programs, recognised by their symbols */
enum glp_kind { KIND_UNKNOWN, KIND_2D_VERT, KIND_2D_FRAG, KIND_3D_VERT, KIND_3D_FRAG };

/* client data in GLP_ATTRIB / GLP_DRAW_ELEMENTS: buffer 0 and these bytes */
#define GLP_INLINE 0xffffffffu
