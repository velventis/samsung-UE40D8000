/*
 * The MICOM (the TV's standby microcontroller, on /dev/ttyS1): it acks
 * each command the TV sends, so TDcSamMicomInterface does not time out.
 *
 * TV -> MICOM: ff ff cmd p0 p1 p2 p3 p4 sum   (sum: cmd + p0..p4)
 * MICOM -> TV: ff ff cmd d0 d1 d2 d3 d4 sum   (the ack: the TV's params)
 *
 * Commands the TV parses as MICOM events (keys, callbacks) are not acked.
 *
 * Remote keys: lines written to the FIFO /hd2/rckey, a key code (hex or
 * decimal) or a name below, go to the TV as the MICOM's IR key packets
 * (ff ff 10 code 0 0 0 0 sum: KeyInputBase's RC key).
 *   echo smarthub > /hd2/rckey
 * And the HD2's own buttons (the htcleo-keypad input device): the Windows
 * button is Smart Hub, the volume rocker the volume keys.
 */
#include "hd2tv.h"

static bool is_event(uint8_t c)
{
    return (c >= 0x10 && c <= 0x12) || (c >= 0x14 && c <= 0x15) ||
           (c >= 0x17 && c <= 0x1a) || c == 0x1c || c == 0x1e || c == 0x96 ||
           c == 0xa0 || c == 0xa6 || c == 0xbf || c == 0xda || c == 0xec;
}

static void *micom_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    uint8_t pkt[9], c;
    int n = 0;

    for (;;) {
        if (syscall(3, fd, &c, 1) != 1) {
            sleep(1);
            continue;
        }
        if (n < 2 && c != 0xff) {
            n = 0;
            continue;
        }
        if (n == 2 && c == 0xff)        /* ff ff ff: resync */
            continue;
        pkt[n++] = c;
        if (n < 9)
            continue;
        n = 0;
        uint8_t sum = 0;
        for (int i = 2; i < 8; i++)
            sum += pkt[i];
        if (sum != pkt[8] || is_event(pkt[2])) {
            if (hd2tv_log_mask & 8)
                hd2tv_log("micom: %02x %02x %02x %02x %02x %02x %02x not acked",
                          pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7], pkt[8]);
            continue;
        }
        if (hd2tv_log_mask & 8)
            hd2tv_log("micom: ack %02x %02x %02x %02x %02x %02x", pkt[2],
                      pkt[3], pkt[4], pkt[5], pkt[6], pkt[7]);
        syscall(4, fd, pkt, 9);
    }
    return 0;
}

static const struct { const char *name; uint8_t code; } keys[] = {
    { "power", 0x02 }, { "source", 0x01 }, { "menu", 0x1a }, { "smarthub", 0x79 },
    { "contents", 0x79 }, { "up", 0x60 }, { "down", 0x61 }, { "left", 0x65 },
    { "right", 0x62 }, { "enter", 0x68 }, { "return", 0x58 }, { "exit", 0x2d },
    { "tools", 0x4b }, { "info", 0x1f }, { "guide", 0x4f }, { "red", 0x6c },
    { "green", 0x14 }, { "yellow", 0x15 }, { "blue", 0x16 }, { "chup", 0x12 },
    { "chdown", 0x10 }, { "volup", 0x07 }, { "voldown", 0x0b }, { "mute", 0x0f },
    { "0", 0x11 }, { "1", 0x04 }, { "2", 0x05 }, { "3", 0x06 }, { "4", 0x08 },
    { "5", 0x09 }, { "6", 0x0a }, { "7", 0x0c }, { "8", 0x0d }, { "9", 0x0e },
};

static int key_code(const char *s)
{
    char *end;
    unsigned long v;

    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!strcmp(s, keys[i].name))
            return keys[i].code;
    v = strtoul(s, &end, 0);
    return end != s && !*end && v < 256 ? (int)v : -1;
}

static void send_key(int master, int k, const char *what)
{
    uint8_t p[9] = { 0xff, 0xff, 0x10, k, 0, 0, 0, 0, 0x10 + k };

    if (hd2tv_log_mask & 8)
        hd2tv_log("micom: remote key %s (0x%02x)", what, k);
    syscall(4, master, p, 9);
}

/* the HD2's buttons (Linux key codes, the kernel's Android keymap) */
static const struct { uint16_t code; uint8_t tv; bool repeat; const char *name; } buttons[] = {
    { 139, 0x79, false, "windows: smarthub" },     /* KEY_MENU: the Windows button */
    { 115, 0x07, true, "volume +" },               /* KEY_VOLUMEUP */
    { 114, 0x0b, true, "volume -" },               /* KEY_VOLUMEDOWN */
};

static int open_keypad(void)
{
    char path[64], buf[64];

    for (int i = 0; i < 16; i++) {
        int fd, n, maj, mnr;
        snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", i);
        if ((fd = syscall(5, path, O_RDONLY, 0)) < 0)
            continue;
        n = syscall(3, fd, buf, sizeof(buf) - 1);
        syscall(6, fd);
        if (n <= 0 || strncmp(buf, "htcleo-keypad", 13))
            continue;
        snprintf(path, sizeof(path), "/sys/class/input/event%d/dev", i);
        if ((fd = syscall(5, path, O_RDONLY, 0)) < 0)
            return -1;
        n = syscall(3, fd, buf, sizeof(buf) - 1);
        syscall(6, fd);
        buf[n > 0 ? n : 0] = 0;
        maj = atoi(buf);
        mnr = atoi(strchr(buf, ':') ? strchr(buf, ':') + 1 : buf);
        unlink("/hd2/keypad");
        if (syscall(14, "/hd2/keypad", 0020600, maj << 8 | mnr) < 0)  /* mknod */
            return -1;
        return syscall(5, "/hd2/keypad", O_RDONLY, 0);
    }
    return -1;
}

static void *keypad_thread(void *arg)
{
    int master = (int)(intptr_t)arg, fd = open_keypad();
    struct { uint32_t sec, usec; uint16_t type, code; int32_t value; } ev;

    if (fd < 0) {
        hd2tv_log("micom: no HD2 keypad");
        return 0;
    }
    int held = -1;                              /* a repeating button down */

    for (;;) {
        /* the keypad driver does not repeat: held buttons that should, do */
        struct { int fd; short events, revents; } pfd = { fd, 1, 0 };
        int r = syscall(168, &pfd, 1, held < 0 ? -1 : 150);         /* poll */
        if (r == 0) {
            send_key(master, buttons[held].tv, buttons[held].name);
            continue;
        }
        if (r < 0 || syscall(3, fd, &ev, sizeof(ev)) != sizeof(ev))
            break;
        if (ev.type != 1)                       /* EV_KEY */
            continue;
        for (int i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); i++) {
            if (ev.code != buttons[i].code)
                continue;
            if (ev.value == 1) {
                send_key(master, buttons[i].tv, buttons[i].name);
                if (buttons[i].repeat) {
                    held = i;
                    /* repeats from 250 ms on, then every 150 ms */
                    struct { int fd; short events, revents; } p2 = { fd, 1, 0 };
                    if (syscall(168, &p2, 1, 250) == 0)
                        send_key(master, buttons[i].tv, buttons[i].name);
                }
            } else if (ev.value == 0 && held == i) {
                held = -1;
            }
        }
    }
    return 0;
}

static void *rckey_thread(void *arg)
{
    int master = (int)(intptr_t)arg;
    char line[64];
    int n = 0;

    unlink("/hd2/rckey");
    if (syscall(14, "/hd2/rckey", 0010666, 0) < 0) {    /* mknod: a FIFO */
        hd2tv_log("micom: no /hd2/rckey");
        return 0;
    }
    for (;;) {
        int fd = syscall(5, "/hd2/rckey", O_RDONLY, 0);
        char c;
        if (fd < 0)
            return 0;
        while (syscall(3, fd, &c, 1) == 1) {
            if (c != '\n' && c != '\r' && c != ' ') {
                if (n < (int)sizeof(line) - 1)
                    line[n++] = c;
                continue;
            }
            line[n] = 0;
            if (n) {
                int k = key_code(line);
                if (k < 0) {
                    hd2tv_log("micom: key %s?", line);
                } else {
                    send_key(master, k, line);
                }
            }
            n = 0;
        }
        syscall(6, fd);
    }
    return 0;
}

void hd2_micom_start(int master)
{
    pthread_t t;

    pthread_create(&t, 0, micom_thread, (void *)(intptr_t)master);
    pthread_create(&t, 0, rckey_thread, (void *)(intptr_t)master);
    pthread_create(&t, 0, keypad_thread, (void *)(intptr_t)master);
}
