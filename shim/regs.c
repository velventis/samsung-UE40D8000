/* TV SoC registers whose reset/status values the firmware waits for */
#include "hd2tv.h"

static const struct { uint32_t addr, val; } init_regs[] = {
    { 0x303f000c, 6 },          /* SE: secure boot authentication done */
};

void hd2_regs_init(void)
{
    for (unsigned i = 0; i < sizeof(init_regs) / sizeof(init_regs[0]); i++) {
        volatile uint32_t *p = hd2tv_phys(init_regs[i].addr, 4);
        if (p) {
            *p = init_regs[i].val;
            munmap((void *)((uintptr_t)p & ~0xfffu), 0x1000);
        }
    }
}
