/* who ends exeDSP: kill() and exit() with their callers */
#include "hd2tv.h"

void hd2tv_where(const char *what, uint32_t a);

int kill(pid_t pid, int sig)
{
    static int (*real)(pid_t, int);

    if (!real)
        real = dlsym(RTLD_NEXT, "kill");
    hd2tv_log("kill(%d, %d) from %p", pid, sig, __builtin_return_address(0));
    hd2tv_where("caller", (uint32_t)__builtin_return_address(0));
    return real(pid, sig);
}

void exit(int code)
{
    static void (*real)(int);

    if (!real)
        real = dlsym(RTLD_NEXT, "exit");
    hd2tv_log("exit(%d) from %p", code, __builtin_return_address(0));
    hd2tv_where("caller", (uint32_t)__builtin_return_address(0));
    real(code);
    for (;;)
        ;
}
