/* hd2daemon PROG ARGS...: run PROG detached (new session, stdio to /dev/null);
 * static, no libc: the TV's busybox has no setsid/nohup */
static long sc(long n, long a, long b, long c)
{
    register long r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c;
    register long r7 __asm__("r7") = n;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

__attribute__((used)) void cmain(long *sp)
{
    long argc = sp[0];
    char **argv = (char **)(sp + 1), **envp = argv + argc + 1;

    if (argc < 2)
        sc(1, 2, 0, 0);
    if (sc(2, 0, 0, 0) == 0) {              /* fork */
        sc(66, 0, 0, 0);                    /* setsid */
        long fd = sc(5, (long)"/dev/null", 2, 0);
        sc(63, fd, 0, 0); sc(63, fd, 1, 0); sc(63, fd, 2, 0);   /* dup2 */
        sc(11, (long)argv[1], (long)(argv + 1), (long)envp);    /* execve */
        sc(1, 127, 0, 0);
    }
    /* let the child leave our process group before adbd reaps it */
    long ts[2] = { 0, 300000000 };
    sc(162, (long)ts, 0, 0);                /* nanosleep */
    sc(1, 0, 0, 0);                         /* exit */
}

__asm__(".globl _start\n_start:\n mov r0, sp\n bl cmain\n");
