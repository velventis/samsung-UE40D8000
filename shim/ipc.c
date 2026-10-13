/* SysV IPC objects of a previous exeDSP (killed, not exited) */
#include "hd2tv.h"

static void clean(const char *path, int kind)
{
    char buf[8192];
    int fd = open(path, O_RDONLY), n, removed = 0;
    char *p, *e;

    if (fd < 0)
        return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    p = strchr(buf, '\n');          /* header */
    while (p && (e = strchr(++p, '\n'))) {
        /* "key id ..." */
        char *q = p;
        while (*q == ' ') q++;
        while (*q && *q != ' ') q++;
        int id = atoi(q);
        if (kind == 0) msgctl(id, IPC_RMID, 0);
        else if (kind == 1) semctl(id, 0, IPC_RMID);
        else shmctl(id, IPC_RMID, 0);
        removed++;
        p = e;
    }
    if (removed)
        hd2tv_log("removed %d stale %s", removed, path);
}

void hd2_ipc_clean(void)
{
    clean("/proc/sysvipc/msg", 0);
    clean("/proc/sysvipc/sem", 1);
    clean("/proc/sysvipc/shm", 2);
}
