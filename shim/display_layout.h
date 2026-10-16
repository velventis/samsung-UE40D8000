#pragma once
/* Pure geometry/pixel helpers, shared with host-side validation. */
struct tv_rect { unsigned x, y, w, h; };
static inline struct tv_rect tv_fit_16_9(unsigned width, unsigned height)
{
    struct tv_rect r = {0, 0, width, width / 16 * 9 + width % 16 * 9 / 16};
    if (r.h > height) { r.h = height; r.w = height / 9 * 16 + height % 9 * 16 / 9; }
    r.x = (width - r.w) / 2;
    r.y = (height - r.h) / 2;
    return r;
}
static inline unsigned tv_channel(unsigned value, unsigned offset, unsigned length)
{
    return length ? ((value * ((1u << length) - 1) + 127) / 255) << offset : 0;
}
