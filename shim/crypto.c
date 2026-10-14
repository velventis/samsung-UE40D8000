/*
 * The SE (security engine)'s ciphers, done by the CPU.
 *
 * libSDAL hands cipher jobs to uldSe_SetCpDesDerived(se, job), which
 * programs the engine and runs its DMA a chunk at a time.  Here the job is
 * done in software instead, for keys given by software (key mode 1); the
 * engine's own keys (hardware key modes) are secret: those jobs get a known
 * answer (efuse_answers) or go to the engine as before (which, emulated,
 * leaves the data as it is).
 *
 * The job (from CCP_HI::hdiEncrypt/hdiDecrypt and the lldSe_* setters):
 *   [0] mode: 0 DES ECB, 1 DES CBC, 2 3DES ECB, 3 3DES CBC,
 *             4/5/6 AES-128 ECB/CBC/CTR, 7/8/9 AES-256 ECB/CBC/CTR
 *   [1] 1 encrypt, 0 decrypt   [2] in   [3] length   [4] out
 *   [5] key   [6] DES: IV; AES: key mode (1: the key at [5])   [7] AES: IV
 */
#include "hd2tv.h"

/* ------------------------------------------------------------------ AES */

static uint8_t sbox[256], inv_sbox[256];

static uint8_t xt(uint8_t x) { return x << 1 ^ (x & 0x80 ? 0x1b : 0); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static void aes_tables(void)
{
    uint8_t p = 1, q = 1;

    if (sbox[0])
        return;
    /* the S-box from the multiplicative inverse and the affine map */
    do {
        p = p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0);
        q ^= q << 1;
        q ^= q << 2;
        q ^= q << 4;
        if (q & 0x80) q ^= 0x09;
        uint8_t x = q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^
                    (q << 4 | q >> 4);
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++)
        inv_sbox[sbox[i]] = i;
}

struct aes { int rounds; uint8_t rk[240]; };

static void aes_key(struct aes *a, const uint8_t *key, int bits)
{
    int nk = bits / 32, total = 4 * ((a->rounds = nk + 6) + 1);
    uint8_t rcon = 1, t[4];

    aes_tables();
    memcpy(a->rk, key, 4 * nk);
    for (int i = nk; i < total; i++) {
        memcpy(t, a->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t u = t[0];
            t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
            rcon = xt(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; k++) t[k] = sbox[t[k]];
        }
        for (int k = 0; k < 4; k++)
            a->rk[4 * i + k] = a->rk[4 * (i - nk) + k] ^ t[k];
    }
}

static void aes_enc(const struct aes *a, uint8_t s[16])
{
    for (int i = 0; i < 16; i++) s[i] ^= a->rk[i];
    for (int r = 1; r <= a->rounds; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++)            /* SubBytes, ShiftRows */
            t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];
        if (r < a->rounds)
            for (int c = 0; c < 4; c++) {       /* MixColumns */
                uint8_t *x = t + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                x[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                x[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                x[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ a->rk[16 * r + i];
    }
}

static void aes_dec(const struct aes *a, uint8_t s[16])
{
    for (int i = 0; i < 16; i++) s[i] ^= a->rk[16 * a->rounds + i];
    for (int r = a->rounds - 1; r >= 0; r--) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++)            /* InvShiftRows, InvSubBytes */
            t[(i + 4 * (i % 4)) % 16] = inv_sbox[s[i]];
        for (int i = 0; i < 16; i++) t[i] ^= a->rk[16 * r + i];
        if (r > 0)
            for (int c = 0; c < 4; c++) {       /* InvMixColumns */
                uint8_t *x = t + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                x[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                x[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                x[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        memcpy(s, t, 16);
    }
}

/* ------------------------------------------------------------------ DES */

static const uint8_t IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const uint8_t FP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
static const uint8_t E[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
static const uint8_t P[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
static const uint8_t PC1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27,
    19, 11, 3, 60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
static const uint8_t PC2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const uint8_t SHIFTS[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const uint8_t S[8][64] = {
    { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
      4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
    { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
      0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
    { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
      13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
      10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
      4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
    { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
      9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
      1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
    { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
      7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 },
};

static uint64_t perm(uint64_t in, const uint8_t *t, int n, int width)
{
    uint64_t r = 0;
    for (int i = 0; i < n; i++)
        r = r << 1 | (in >> (width - t[i]) & 1);
    return r;
}

struct des { uint64_t k[16]; };

static void des_key(struct des *d, const uint8_t *key)
{
    uint64_t k = 0, cd;
    uint32_t c, dd;

    for (int i = 0; i < 8; i++) k = k << 8 | key[i];
    cd = perm(k, PC1, 56, 64);
    c = cd >> 28;
    dd = cd & 0xfffffff;
    for (int i = 0; i < 16; i++) {
        for (int s = 0; s < SHIFTS[i]; s++) {
            c = (c << 1 | c >> 27) & 0xfffffff;
            dd = (dd << 1 | dd >> 27) & 0xfffffff;
        }
        d->k[i] = perm((uint64_t)c << 28 | dd, PC2, 48, 56);
    }
}

static uint64_t des_block(const struct des *d, uint64_t b, bool dec)
{
    uint64_t x = perm(b, IP, 64, 64);
    uint32_t l = x >> 32, r = x;

    for (int i = 0; i < 16; i++) {
        uint64_t e = perm(r, E, 48, 32) ^ d->k[dec ? 15 - i : i];
        uint32_t f = 0;
        for (int s = 0; s < 8; s++) {
            int six = e >> (42 - 6 * s) & 63;
            f = f << 4 | S[s][(six & 0x20) | (six & 1) << 4 | (six >> 1 & 15)];
        }
        f = perm(f, P, 32, 32);
        uint32_t t = l ^ f;
        l = r;
        r = t;
    }
    return perm((uint64_t)r << 32 | l, FP, 64, 64);
}

/* ------------------------------------------------------------------ jobs */

struct cipher {
    int block;
    bool aes, tdes;
    struct aes a;
    struct des d[3];
};

static void block(const struct cipher *c, uint8_t *b, bool dec)
{
    if (c->aes) {
        if (dec) aes_dec(&c->a, b);
        else aes_enc(&c->a, b);
        return;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | b[i];
    if (c->tdes)    /* EDE: K1, K2, K3 (decrypt: the reverse) */
        v = dec ? des_block(&c->d[0], des_block(&c->d[1], des_block(&c->d[2], v, true), false), true)
                : des_block(&c->d[2], des_block(&c->d[1], des_block(&c->d[0], v, false), true), false);
    else
        v = des_block(&c->d[0], v, dec);
    for (int i = 7; i >= 0; i--) { b[i] = v; v >>= 8; }
}

/* in place is fine (in == out) */
static void run(const struct cipher *c, int chain, bool dec, const uint8_t *in,
                uint8_t *out, uint32_t len, uint8_t *iv)
{
    int bs = c->block;
    uint8_t b[16], prev[16], ks[16];

    for (uint32_t off = 0; off < len; off += bs) {
        uint32_t n = len - off < (uint32_t)bs ? len - off : (uint32_t)bs;
        memset(b, 0, bs);
        memcpy(b, in + off, n);
        if (chain == 2) {                       /* CTR: big-endian counter */
            memcpy(ks, iv, bs);
            block(c, ks, false);
            for (uint32_t i = 0; i < n; i++) out[off + i] = b[i] ^ ks[i];
            for (int i = bs - 1; i >= 0 && !++iv[i]; i--)
                ;
            continue;
        }
        if (chain == 1 && !dec)
            for (int i = 0; i < bs; i++) b[i] ^= iv[i];
        memcpy(prev, b, bs);
        block(c, b, dec);
        if (chain == 1 && dec) {
            for (int i = 0; i < bs; i++) b[i] ^= iv[i];
            memcpy(iv, prev, bs);
        } else if (chain == 1)
            memcpy(iv, b, bs);
        memcpy(out + off, b, n);
    }
}

/*
 * The engine's own (efuse) key cannot be had, but what it turns some
 * inputs into is known.  exeDSP's widget "common key" (DecryptWithCommonKey,
 * for Smart Hub's *.cmk files) is a 32-byte key and IV that the engine
 * decrypts with its efuse key; the result is the AES-128-CBC key and IV
 * SamyGO published for .cmk files (forum.samygo.tv, viewtopic.php?t=4994).
 */
static const struct { uint8_t in[32], out[32]; } efuse_answers[] = {
    { { 0xe0, 0xc3, 0x04, 0xf5, 0x14, 0xb9, 0xb6, 0xfc, 0xe2, 0x87, 0x07, 0x2f, 0x09, 0x8a, 0x93, 0xdd,
        0x80, 0x46, 0xd3, 0xce, 0x25, 0xd8, 0xa3, 0xac, 0xa7, 0x3c, 0xde, 0x34, 0x24, 0x07, 0x08, 0xd6 },
      { 0xb1, 0xd5, 0xf1, 0x22, 0xe7, 0x5d, 0x75, 0x7c, 0x79, 0xf4, 0x88, 0x86, 0xd4, 0x2f, 0x8e, 0x1a,
        0xbf, 0xe9, 0x32, 0xf9, 0x27, 0x3d, 0xc2, 0xa0, 0xdf, 0xc9, 0x3f, 0x0b, 0x8e, 0x7a, 0xc7, 0xc2 } },
};

static bool efuse_answer(const uint32_t *job)
{
    const uint8_t *in = (const uint8_t *)(uintptr_t)job[2];
    uint8_t *out = (uint8_t *)(uintptr_t)job[4];

    if (job[1] || job[3] != 32 || !in || !out)
        return false;
    for (unsigned i = 0; i < sizeof(efuse_answers) / sizeof(efuse_answers[0]); i++)
        if (!memcmp(in, efuse_answers[i].in, 32)) {
            memcpy(out, efuse_answers[i].out, 32);
            return true;
        }
    return false;
}

uint32_t uldSe_SetCpDesDerived(void *se, uint32_t *job)
{
    static uint32_t (*real)(void *, uint32_t *);
    struct cipher c;
    uint32_t mode = job[0], len = job[3];
    bool dec = !job[1];
    uint8_t iv[16] = { 0 };
    int chain;

    if (!real)
        real = (uint32_t (*)(void *, uint32_t *))dlsym(RTLD_NEXT, "uldSe_SetCpDesDerived");
    memset(&c, 0, sizeof(c));
    if (mode <= 3) {                            /* DES, 3DES */
        const uint8_t *key = (const uint8_t *)(uintptr_t)job[5];
        if (!key)
            return real(se, job);
        c.block = 8;
        c.tdes = mode >= 2;
        for (int i = 0; i < (c.tdes ? 3 : 1); i++)
            des_key(&c.d[i], key + 8 * i);
        chain = mode & 1;
        if (chain && job[6])
            memcpy(iv, (const void *)(uintptr_t)job[6], 8);
    } else if (mode <= 9) {                     /* AES */
        if (job[6] != 1 && efuse_answer(job)) {
            hd2tv_log("se: the efuse key's answer for a known input");
            return 0;
        }
        if (job[6] != 1 || !job[5]) {
            static bool told;
            if (!told && (told = true))
                hd2tv_log("se: AES with the engine's own key (mode %u, key mode %u)",
                          mode, job[6]);
            return real(se, job);
        }
        c.block = 16;
        c.aes = true;
        aes_key(&c.a, (const uint8_t *)(uintptr_t)job[5], mode >= 7 ? 256 : 128);
        chain = (mode - 4) % 3;
        if (chain && job[7])
            memcpy(iv, (const void *)(uintptr_t)job[7], 16);
    } else
        return real(se, job);
    if (!job[2] || !job[4])
        return 0x86002f04;
    if ((hd2tv_log_mask & 0x200) && dec) {
        const uint8_t *k = (const uint8_t *)(uintptr_t)job[5], *in = (const uint8_t *)(uintptr_t)job[2];
        char h[200];
        int n = 0;
        for (int i = 0; i < (c.aes ? (mode >= 7 ? 32 : 16) : c.tdes ? 24 : 8); i++)
            n += snprintf(h + n, sizeof(h) - n, "%02x", k[i]);
        n += snprintf(h + n, sizeof(h) - n, " iv ");
        for (int i = 0; i < c.block; i++)
            n += snprintf(h + n, sizeof(h) - n, "%02x", iv[i]);
        n += snprintf(h + n, sizeof(h) - n, " in ");
        for (int i = 0; i < 16 && (uint32_t)i < len; i++)
            n += snprintf(h + n, sizeof(h) - n, "%02x", in[i]);
        hd2tv_log("se: key %s, from %p", h, __builtin_return_address(0));
        if (hd2tv_log_mask & 0x400) {           /* exeDSP return addresses */
            uint32_t *sp = (uint32_t *)__builtin_frame_address(0);
            n = 0;
            for (int i = 0; i < 400 && n < (int)sizeof(h) - 12; i++)
                if (sp[i] >= 0x8000 && sp[i] < 0x5c00000 && (sp[i] & 3) == 0)
                    n += snprintf(h + n, sizeof(h) - n, " %x", sp[i]);
            hd2tv_log("se: stack%s", h);
        }
    }
    run(&c, chain, dec && chain != 2, (const uint8_t *)(uintptr_t)job[2],
        (uint8_t *)(uintptr_t)job[4], len, iv);
    if (hd2tv_log_mask & 0x200)
        hd2tv_log("se: mode %u %s %u bytes", mode, dec ? "decrypt" : "encrypt", len);
    return 0;
}

/* AES-128 known answers (FIPS-197 C.1, SP 800-38A F.2.1), DES (FIPS 81):
 * logged at start */
void hd2_crypto_selftest(void)
{
    static const uint8_t k[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t ct[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                    0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    uint8_t b[16];
    struct aes a;
    struct des d;
    bool ok;

    for (int i = 0; i < 16; i++) b[i] = i * 0x11;
    aes_key(&a, k, 128);
    aes_enc(&a, b);
    ok = !memcmp(b, ct, 16);
    aes_dec(&a, b);
    for (int i = 0; i < 16; i++) ok &= b[i] == i * 0x11;
    static const uint8_t dk[8] = { 0x13, 0x34, 0x57, 0x79, 0x9b, 0xbc, 0xdf, 0xf1 };
    des_key(&d, dk);
    uint64_t v = des_block(&d, 0x0123456789abcdefull, false);
    ok &= v == 0x85e813540f0ab405ull;
    ok &= des_block(&d, v, true) == 0x0123456789abcdefull;
    if (!ok)
        hd2tv_log("se: cipher self test FAILED");
}
