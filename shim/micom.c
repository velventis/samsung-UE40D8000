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
                    uint8_t p[9] = { 0xff, 0xff, 0x10, k, 0, 0, 0, 0, 0x10 + k };
                    hd2tv_log("micom: remote key %s (0x%02x)", line, k);
                    syscall(4, master, p, 9);
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
}
