/*
 * The MICOM (the TV's standby microcontroller, on /dev/ttyS1): it acks
 * each command the TV sends, so TDcSamMicomInterface does not time out.
 *
 * TV -> MICOM: ff ff cmd p0 p1 p2 p3 p4 sum   (sum: cmd + p0..p4)
 * MICOM -> TV: ff ff cmd d0 d1 d2 d3 d4 sum   (the ack: the TV's params)
 *
 * Commands the TV parses as MICOM events (keys, callbacks) are not acked.
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

void hd2_micom_start(int master)
{
    pthread_t t;

    pthread_create(&t, 0, micom_thread, (void *)(intptr_t)master);
}
