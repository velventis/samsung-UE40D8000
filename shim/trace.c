/* return codes of libSDAL internals, for finding what fails */
#include "hd2tv.h"

typedef uint32_t (*fn8)(uint32_t, uint32_t, uint32_t, uint32_t,
                        uint32_t, uint32_t, uint32_t, uint32_t);

#define TRACE(name)                                                         \
    uint32_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t d,           \
                  uint32_t e, uint32_t f, uint32_t g, uint32_t h)           \
    {                                                                       \
        static fn8 real;                                                    \
        uint32_t r;                                                         \
        if (!real)                                                          \
            real = (fn8)dlsym(RTLD_NEXT, #name);                            \
        r = real(a, b, c, d, e, f, g, h);                                   \
        if (hd2tv_log_mask & 32)                                            \
            hd2tv_log("%s(%x, %x, %x, %x) = %x", #name, a, b, c, d, r);     \
        return r;                                                           \
    }

#include "trace.inc"
