#include <assert.h>
#include <stdio.h>
#include "display_layout.h"
int main(void)
{
    struct tv_rect r = tv_fit_16_9(1920, 1080);
    assert(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080);
    r = tv_fit_16_9(1280, 800);
    assert(r.x == 0 && r.y == 40 && r.w == 1280 && r.h == 720);
    r = tv_fit_16_9(1024, 768);
    assert(r.x == 0 && r.y == 96 && r.w == 1024 && r.h == 576);
    r = tv_fit_16_9(2560, 1080);
    assert(r.x == 320 && r.y == 0 && r.w == 1920 && r.h == 1080);
    r = tv_fit_16_9(480, 800);
    assert(r.x == 0 && r.y == 265 && r.w == 480 && r.h == 270);
    assert(tv_channel(255, 11, 5) == 0xf800);
    assert(tv_channel(255, 5, 6) == 0x07e0);
    assert(tv_channel(255, 0, 5) == 0x001f);
    assert(tv_channel(255, 16, 8) == 0xff0000);
    assert(tv_channel(255, 0, 8) == 0xff);
    assert(tv_channel(255, 0, 0) == 0);
    puts("display tests passed: native 1080p, 16:10/4:3/ultrawide/portrait letterboxing, RGB565/XRGB8888");
}
