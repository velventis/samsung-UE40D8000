/*
 * libhd2tv.so: the Samsung UE40D8000 (Genoa-P, SDP1004) firmware on an
 * HTC HD2.  Preloaded into exeDSP, it stands in for what the TV kernel
 * and samdrv.ko gave the firmware's libSDAL:
 *
 *   /dev/mem         physical memory of the TV SoC: its DDR banks and
 *                    register blocks, backed by a sparse file on tmpfs
 *   /dev/sam/<dev>N  samdrv.ko's devices (ioctl/mmap emulated)
 *   /dev/sdp_*       the TV kernel's own drivers (GPIO, I2C, hwmem)
 *   /dev/ttyS1       the MICOM, which acks the TV's commands
 *
 * HD2TV_LOG=<mask> selects the log (1 opens, 2 ioctls, 4 mmaps, 8 MICOM,
 * 0x200 SE cipher jobs).
 */
#include "tvlibc.h"
#include "hd2tv.h"

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_close)(int);
static int (*real_ioctl)(int, unsigned long, void *);
static void *(*real_mmap64)(void *, size_t, int, int, int, off64_t);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);

unsigned hd2tv_log_mask = 5 | 32;
static int phys_fd = -1;             /* backing store of /dev/mem */

#define MAX_FD 1024
static const struct hd2_dev *fake[MAX_FD];
static int fake_unit[MAX_FD];
static bool is_phys[MAX_FD];        /* a dup of phys_fd (/dev/mem) */

/* exeDSP's mappings of the TV's memory, for hd2tv_va2phys */
static struct { uintptr_t va; uint32_t len; uint64_t pa; } maps[512];
static int nmaps;
static pthread_mutex_t maps_lock = PTHREAD_MUTEX_INITIALIZER;

static void map_note(void *va, size_t len, uint64_t pa)
{
    pthread_mutex_lock(&maps_lock);
    int i;
    for (i = 0; i < nmaps && maps[i].va != (uintptr_t)va; i++)
        ;
    if (i == nmaps && nmaps < 512)
        nmaps++;
    if (i < 512) {
        maps[i].va = (uintptr_t)va;
        maps[i].len = len;
        maps[i].pa = pa;
    }
    pthread_mutex_unlock(&maps_lock);
}

/* the TV physical address of exeDSP's virtual address va, or ~0 */
uint64_t hd2tv_va2phys(const void *va)
{
    uint64_t pa = ~0ull;
    pthread_mutex_lock(&maps_lock);
    for (int i = nmaps - 1; i >= 0; i--)
        if ((uintptr_t)va >= maps[i].va && (uintptr_t)va - maps[i].va < maps[i].len) {
            pa = maps[i].pa + ((uintptr_t)va - maps[i].va);
            break;
        }
    pthread_mutex_unlock(&maps_lock);
    return pa;
}

int hd2tv_phys_fd(void)
{
    return phys_fd;
}

void hd2tv_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n = snprintf(buf, sizeof(buf), "[hd2tv] ");

    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    if (buf[n - 1] != '\n')
        buf[n++] = '\n';
    real_write(2, buf, n);
}

static void init_real(void)
{
    if (real_open)
        return;
    real_open = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_close = dlsym(RTLD_NEXT, "close");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_mmap64 = dlsym(RTLD_NEXT, "mmap64");
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
}

__attribute__((constructor)) static void hd2tv_init(void)
{
    const char *e;

    init_real();
    /* only exeDSP itself: not the shells and tools it starts */
    unsetenv("LD_PRELOAD");
    e = getenv("HD2TV_LOG");
    if (e)
        hd2tv_log_mask = strtoul(e, 0, 0);
    /* the TV's physical address space, sparse */
    unlink(HD2TV_PHYS_FILE);
    phys_fd = real_open(HD2TV_PHYS_FILE, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (phys_fd < 0 || ftruncate64(phys_fd, 0x100000000ll - 0x1000) < 0)
        hd2tv_log("cannot create %s", HD2TV_PHYS_FILE);
    unlink(HD2TV_PHYS_FILE);
    setvbuf(stdout, 0, _IOLBF, 0);
    hd2tv_log("loaded, pid %d", getpid());
    hd2_ipc_clean();
    {
        /* the factory MAC change is done (no MICOM to go through) */
        int fd = real_open("/mtd_rwarea/MAC_CHANGE", O_RDWR | O_CREAT, 0644);
        if (fd >= 0)
            real_close(fd);
        /* no CI+ (no CAM, no credentials): its services database cannot
         * be opened, so the CI+ storage stays off */
        unlink("/mtd_rwarea/cip_services_database");
        mkdir("/mtd_rwarea/cip_services_database", 0755);
    }
    hd2_devices_init();
    hd2_regs_init();
    hd2tv_crash_init();
    hd2_crypto_selftest();
    hd2_fb_init();
}

void *hd2tv_phys(uint32_t pa, uint32_t len)
{
    void *p = real_mmap64(0, len + (pa & 0xfff), PROT_READ | PROT_WRITE, MAP_SHARED, phys_fd,
                          pa & ~0xfffu);

    return p == MAP_FAILED ? 0 : (char *)p + (pa & 0xfff);
}

/* ---------------------------------------------------------------- open */

static int open_common(const char *path, int flags, mode_t mode, bool is64)
{
    const struct hd2_dev *d;
    int unit, fd;

    init_real();
    char img_buf[256];
    path = hd2_image_path(path, img_buf, sizeof(img_buf));
    if (path && !strcmp(path, "/dev/mem")) {
        fd = dup(phys_fd);
        if (fd >= 0 && fd < MAX_FD)
            is_phys[fd] = true;
        if (hd2tv_log_mask & 1)
            hd2tv_log("open /dev/mem -> %d", fd);
        return fd;
    }
    if (path && (!strcmp(path, "/dev/ttyS1") || !strcmp(path, "/dev/ttyS2") || !strcmp(path, "/dev/ttyS3"))) {
        /* the MICOM's and the debug UART: ptys whose masters stay open;
         * the MICOM's answers acks, the debug UART's stays silent */
        int master = -1;
        int lock = 0, n = -1;
        char pts[32];

        if (master < 0)
            master = real_open("/dev/ptmx", O_RDWR);
        if (master >= 0 && real_ioctl(master, 0x40045431, &lock) == 0 &&
            real_ioctl(master, 0x80045430, &n) == 0) {
            snprintf(pts, sizeof(pts), "/dev/pts/%d", n);
            fd = real_open(pts, flags, mode);
            if (!strcmp(path, "/dev/ttyS1"))
                hd2_micom_start(master);
            else
                hd2_debug_uart_start(master);
            hd2tv_log("open %s -> %d (%s%s)", path, fd, pts,
                      strcmp(path, "/dev/ttyS1") ? ", silent" : ", MICOM");
            return fd;
        }
    }
    d = path ? hd2_dev_find(path, &unit) : 0;
    if (!d) {
        fd = is64 ? real_open64(path, flags, mode) : real_open(path, flags, mode);
        if (fd < 0 && path && !strncmp(path, "/dev/", 5) && (hd2tv_log_mask & 1))
            hd2tv_log("open %s failed", path);
        return fd;
    }
    fd = real_open("/dev/null", O_RDWR);
    if (fd >= 0 && fd < MAX_FD) {
        fake[fd] = d;
        fake_unit[fd] = unit;
    }
    if ((hd2tv_log_mask & 1) && d != &hd2_dev_gpio && d != &hd2_dev_i2c)
        hd2tv_log("open %s -> %d", path, fd);
    if (d->open)
        d->open(unit);
    return fd;
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    mode_t mode;

    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
    return open_common(path, flags, mode, false);
}

int open64(const char *path, int flags, ...)
{
    va_list ap;
    mode_t mode;

    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
    return open_common(path, flags, mode, true);
}

int close(int fd)
{
    init_real();
    if (fd >= 0 && fd < MAX_FD) {
        fake[fd] = 0;
        is_phys[fd] = false;
    }
    return real_close(fd);
}

/* --------------------------------------------------------------- ioctl */

/* log every (device, request) once */
static bool first_seen(const struct hd2_dev *d, unsigned long req)
{
    static struct { const struct hd2_dev *d; unsigned long req; } seen[4096];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    unsigned h = ((uintptr_t)d * 31 + req) % 4096;
    bool r = false;

    pthread_mutex_lock(&lock);
    for (int i = 0; i < 4096; i++, h = (h + 1) % 4096) {
        if (seen[h].d == d && seen[h].req == req)
            break;
        if (!seen[h].d) {
            seen[h].d = d;
            seen[h].req = req;
            r = true;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    const struct hd2_dev *d;
    int r;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    init_real();
    d = fd >= 0 && fd < MAX_FD ? fake[fd] : 0;
    if (!d)
        return real_ioctl(fd, req, arg);
    r = d->ioctl ? d->ioctl(fake_unit[fd], req, arg) : 0;
    if ((hd2tv_log_mask & 2) || first_seen(d, req))
        hd2tv_log("ioctl %s%d 0x%lx(%c,%d,nr %d,sz %d) %p -> %d", d->name,
                  fake_unit[fd], req, (int)((req >> 8) & 0xff) >= 32 ?
                  (int)((req >> 8) & 0xff) : '?', (int)(req >> 30),
                  (int)(req & 0xff), (int)((req >> 16) & 0x3fff), arg, r);
    return r;
}

/* ---------------------------------------------------------------- mmap */

static void *mmap_common(void *addr, size_t len, int prot, int flags, int fd,
                         uint64_t off)
{
    const struct hd2_dev *d;
    void *p;

    init_real();
    d = fd >= 0 && fd < MAX_FD ? fake[fd] : 0;
    if (d) {
        /* samdrv's mmaps are of physical memory too */
        uint64_t pa = d->mmap ? d->mmap(fake_unit[fd], off, len) : off;

        p = real_mmap64(addr, len, prot, (flags & ~MAP_PRIVATE) | MAP_SHARED,
                        phys_fd, pa);
        if (hd2tv_log_mask & 4)
            hd2tv_log("mmap %s%d off 0x%llx len 0x%x -> pa 0x%llx %p",
                      d->name, fake_unit[fd], off, len, pa, p);
        if (p != MAP_FAILED)
            map_note(p, len, pa);
        return p;
    }
    p = real_mmap64(addr, len, prot, flags, fd, off);
    if (p != MAP_FAILED && fd >= 0 && fd < MAX_FD && is_phys[fd])
        map_note(p, len, off);
    if ((hd2tv_log_mask & 4) && off >= 0x10000000)
        hd2tv_log("mmap fd %d off 0x%llx len 0x%x -> %p", fd, off, len, p);
    return p;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    return mmap_common(addr, len, prot, flags, fd, (uint32_t)off);
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off64_t off)
{
    return mmap_common(addr, len, prot, flags, fd, off);
}

ssize_t read(int fd, void *buf, size_t n)
{
    const struct hd2_dev *d;

    init_real();
    d = fd >= 0 && fd < MAX_FD ? fake[fd] : 0;
    if (d) {
        if (d->read)
            return d->read(fake_unit[fd], buf, n);
        /* samdrv's reads wait for the device's interrupts: none come */
        hd2tv_log("read %s%d %d: blocking", d->name, fake_unit[fd], n);
        for (;;)
            sleep(3600);
    }
    ssize_t r = real_read(fd, buf, n);
    if (r <= 0) {
        /* who polls a dead fd? (once per caller) */
        static void *seen[16];
        void *c = __builtin_return_address(0);
        int i;
        for (i = 0; i < 16 && seen[i] && seen[i] != c; i++)
            ;
        if (i < 16 && !seen[i]) {
            char p[32], l[128];
            int k;
            seen[i] = c;
            snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
            k = readlink(p, l, sizeof(l) - 1);
            l[k > 0 ? k : 0] = 0;
            hd2tv_log("read(%d %s) = %d from %p", fd, l, (int)r, c);
            hd2tv_where("caller", (uint32_t)c);
        }
    }
    return r;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    const struct hd2_dev *d;

    init_real();
    d = fd >= 0 && fd < MAX_FD ? fake[fd] : 0;
    if (d) {
        if (d->write)
            return d->write(fake_unit[fd], buf, n);
        hd2tv_log("write %s%d %d", d->name, fake_unit[fd], n);
        return n;
    }
    return real_write(fd, buf, n);
}

/* --------------------------------------------------------------- times */

/*
 * times(): on 32-bit ARM, jiffies start 5 minutes before 0, so the clock
 * ticks the kernel returns pass through -4095..-1 about 5 minutes after
 * boot, which glibc 2.11 takes for an error (EFAULT) and answers by
 * touching the buffer: PCTime::Tick (times(NULL)) crashes.  Here the ticks
 * (100 Hz, as sysconf(_SC_CLK_TCK)) come from CLOCK_MONOTONIC instead.
 */
struct tms_ { long utime, stime, cutime, cstime; };

long times(struct tms_ *buf)
{
    struct timespec ts;

    if (buf)
        syscall(43, buf);                       /* sys_times fills it */
    syscall(263, CLOCK_MONOTONIC, &ts);         /* clock_gettime */
    return 1000 + ts.tv_sec * 100 + ts.tv_nsec / 10000000;
}
