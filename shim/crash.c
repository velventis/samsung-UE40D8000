/* report crashes of exeDSP: registers and the mapping they are in */
#include "hd2tv.h"

static void where(const char *what, uint32_t a)
{
    char buf[8192], *p, *e;
    int fd = open("/proc/self/maps", O_RDONLY);
    int n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : 0;

    /* /proc/self/maps can be long: scan it in chunks */
    while (n > 0) {
        buf[n] = 0;
        for (p = buf; (e = strchr(p, '\n')); p = e + 1) {
            uint32_t lo = strtoul(p, 0, 16), hi = strtoul(strchr(p, '-') + 1, 0, 16);
            *e = 0;
            if (a >= lo && a < hi) {
                char *name = strrchr(p, ' ');
                uint32_t off = strtoul(strchr(strchr(p, ' ') + 1, ' ') + 1, 0, 16);
                hd2tv_log("  %s %08x = %s +0x%x", what, a, name ? name + 1 : "?",
                          a - lo + off);
                close(fd);
                return;
            }
        }
        /* keep the partial line */
        int keep = strlen(p);
        memcpy(buf, p, keep);
        n = read(fd, buf + keep, sizeof(buf) - 1 - keep);
        if (n > 0)
            n += keep;
    }
    hd2tv_log("  %s %08x", what, a);
    if (fd >= 0)
        close(fd);
}

static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    uint32_t *uc = ctx;
    uint32_t *r = uc + 8;       /* uc_mcontext.arm_r0 */

    hd2tv_log("signal %d at %p, tid %ld", sig, si->si_addr, syscall(224));
    hd2tv_log("  r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x", r[0], r[1], r[2], r[3], r[4], r[5]);
    hd2tv_log("  r6 %08x r7 %08x r8 %08x r9 %08x r10 %08x fp %08x", r[6], r[7], r[8], r[9], r[10], r[11]);
    hd2tv_log("  ip %08x sp %08x lr %08x pc %08x cpsr %08x", r[12], r[13], r[14], r[15], r[16]);
    where("pc", r[15]);
    where("lr", r[14]);
    for (int i = 0, k = 0; i < 256 && k < 12; i++) {
        uint32_t v = ((uint32_t *)r[13])[i];
        if ((v >= 0x8000 && v < 0x7460000) || (v >= 0x40000000 && v < 0x60000000)) {
            char w[16];
            snprintf(w, sizeof(w), "sp+%x", i * 4);
            where(w, v);
            k++;
        }
    }
    _exit(128 + sig);
}

void hd2tv_crash_init(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(4, &sa, 0);       /* SIGILL */
}
