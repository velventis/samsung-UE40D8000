/* memdump PID ADDR LEN > out: read another process's memory (static) */
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    register long r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d, r4 __asm__("r4") = e, r5 __asm__("r5") = f;
    register long r7 __asm__("r7") = n;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7) : "memory");
    return r0;
}

static unsigned long num(const char *s)
{
    unsigned long v = 0, base = 10;
    if (s[0] == '0' && s[1] == 'x') { base = 16; s += 2; }
    for (; *s; s++)
        v = v * base + (*s >= 'a' ? *s - 'a' + 10 : *s >= 'A' ? *s - 'A' + 10 : *s - '0');
    return v;
}

static char buf[65536];

__attribute__((used)) void cmain(long *sp)
{
    char **argv = (char **)(sp + 1), path[32] = "/proc/";
    int i = 6;
    const char *p = argv[1];
    unsigned long addr = num(argv[2]), len = num(argv[3]);

    while (*p) path[i++] = *p++;
    for (p = "/mem"; *p; ) path[i++] = *p++;
    path[i] = 0;
    long pid = num(argv[1]);
    /* 2.6.32 lets only the tracer read /proc/PID/mem */
    sc(26, 16, pid, 0, 0, 0, 0);                    /* PTRACE_ATTACH */
    sc(114, pid, 0, 0x40000000, 0, 0, 0);           /* wait4 __WALL */
    long fd = sc(5, (long)path, 0, 0, 0, 0, 0);
    while (len > 0 && fd >= 0) {
        unsigned long n = len > sizeof(buf) ? sizeof(buf) : len;
        /* pread64(fd, buf, n, pad, lo, hi) on ARM EABI */
        long r = sc(180, fd, (long)buf, n, 0, (long)addr, 0);
        if (r <= 0) break;
        sc(4, 1, (long)buf, r, 0, 0, 0);
        addr += r; len -= r;
    }
    sc(26, 17, pid, 0, 0, 0, 0);                    /* PTRACE_DETACH */
    sc(1, 0, 0, 0, 0, 0, 0);
}
__asm__(".globl _start\n_start:\n mov r0, sp\n bl cmain\n");
