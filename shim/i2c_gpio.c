/* the TV kernel's sdp_i2c and sdp_gpio drivers */
#include "hd2tv.h"

/* ---------------------------------------------------------------- I2C */
struct sdp_i2c {
    uint8_t slave;              /* 8-bit address */
    uint8_t subaddr_len;        /* 0-3 bytes, big endian */
    uint16_t speed;             /* kHz */
    uint16_t unknown;
    uint16_t pad;
    uint32_t len;
    uint8_t *subaddr;
    uint8_t *data;
};

/* every slave answers; EEPROMs (0xA0-0xAE) remember, in /mtd_rwarea */
#define EEPROM_SIZE 0x10000
struct i2c_slave {
    uint8_t port, slave;
    uint8_t *mem;
    uint32_t ptr;
};
static struct i2c_slave slaves[64];
static pthread_mutex_t i2c_lock = PTHREAD_MUTEX_INITIALIZER;

static struct i2c_slave *slave_get(int port, int addr)
{
    int i;

    for (i = 0; i < 64 && slaves[i].mem; i++)
        if (slaves[i].port == port && slaves[i].slave == addr)
            return &slaves[i];
    if (i == 64)
        return &slaves[63];
    slaves[i].port = port;
    slaves[i].slave = addr;
    if ((addr & 0xf0) == 0xa0) {
        char path[64];
        int fd;

        snprintf(path, sizeof(path), "/mtd_rwarea/hd2-eeprom-%d-%02x.bin",
                 port, addr);
        fd = open(path, O_RDWR | O_CREAT, 0644);
        if (fd >= 0) {
            off_t n = lseek(fd, 0, 2);
            if (n < EEPROM_SIZE) {
                /* blank (0xff crashes the EEPROM store) */
                static uint8_t ff[4096];
                memset(ff, 0, sizeof(ff));
                lseek(fd, n, SEEK_SET);
                while (n < EEPROM_SIZE) {
                    write(fd, ff, sizeof(ff) - (n % sizeof(ff)));
                    n += sizeof(ff) - (n % sizeof(ff));
                }
            }
            slaves[i].mem = mmap(0, EEPROM_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
            close(fd);
            if (slaves[i].mem == MAP_FAILED)
                slaves[i].mem = 0;
        }
        hd2tv_log("i2c %d:%02x: EEPROM %s", port, addr, path);
    }
    if (!slaves[i].mem)
        slaves[i].mem = calloc(1, EEPROM_SIZE);
    return &slaves[i];
}

static uint32_t subaddr(const struct sdp_i2c *t)
{
    uint32_t a = 0;

    for (int i = 0; i < t->subaddr_len && i < 3; i++)
        a = a << 8 | t->subaddr[i];
    return a;
}

static int i2c_ioctl(int unit, unsigned long req, void *arg)
{
    struct sdp_i2c *t = arg;
    struct i2c_slave *s;
    uint32_t a;

    pthread_mutex_lock(&i2c_lock);
    s = slave_get(unit, t->slave & 0xfe);
    a = s->ptr;
    switch (req) {
    case 1:                     /* write, the subaddress first */
        if (t->subaddr_len)
            a = subaddr(t);
        for (uint32_t i = 0; i < t->len; i++)
            s->mem[(a + i) % EEPROM_SIZE] = t->data[i];
        s->ptr = a + t->len;
        break;
    case 0x11:                  /* subaddress, repeated start, read */
    case 0x12:                  /* write, then read */
        a = subaddr(t);
        /* fall through */
    case 2:
        for (uint32_t i = 0; i < t->len; i++)
            t->data[i] = s->mem[(a + i) % EEPROM_SIZE];
        s->ptr = a + t->len;
        break;
    default:
        hd2tv_log("i2c ioctl 0x%lx?", req);
    }
    if (hd2tv_log_mask & 8)
        hd2tv_log("i2c %lx %d:%02x @%x len %d [%02x %02x]", req, unit, t->slave, a, t->len, t->len ? t->data[0] : 0,
                  t->len > 1 ? t->data[1] : 0);
    pthread_mutex_unlock(&i2c_lock);
    return 0;
}

/* --------------------------------------------------------------- GPIO */
struct sdp_gpio {
    uint16_t pin;               /* port << 4 | bit */
    uint16_t pad;
    uint8_t val;
};

static uint8_t gpio_out[32 * 16];
static uint8_t gpio_in[32 * 16];

void hd2tv_gpio_set_input(int port, int bit, int val)
{
    gpio_in[(port << 4 | bit) % (32 * 16)] = val;
}

static int gpio_ioctl(int unit, unsigned long req, void *arg)
{
    struct sdp_gpio *g = arg;
    unsigned pin = g->pin % (32 * 16);

    switch (req) {
    case 0x4a:                  /* configure */
        break;
    case 0x4b:
        gpio_out[pin] = g->val;
        gpio_in[pin] = g->val;
        break;
    case 0x4c:
        g->val = gpio_in[pin];
        break;
    }
    if (hd2tv_log_mask & 16)
        hd2tv_log("gpio %lx P%d.%d = %d", req, pin >> 4, pin & 15, g->val);
    return 0;
}

const struct hd2_dev hd2_dev_i2c = { "sdp_i2c", "/dev/sdp_i2c0", 0, i2c_ioctl };
const struct hd2_dev hd2_dev_m_i2c = { "sdp_m_i2c", "/dev/sdp_m_i2c0", 0, i2c_ioctl };
const struct hd2_dev hd2_dev_gpio = { "sdp_gpio", "/dev/sdp_gpio", 0, gpio_ioctl };
