/*
 * nandecc: correct a raw dump of the Samsung TV's MLC NAND (UE46D5520).
 *
 * Geometry: blocks of 128 pages, pages of 8192 data + 436 spare bytes.
 * The spare holds 8 slots of [12 free OOB bytes][42 ECC bytes] + 4 pad.
 * ECC: binary BCH, GF(2^14) with x^14+x^5+x^3+x+1 (0x402b), t = 24
 * (roots alpha^1..alpha^48), over 1078-byte codewords (1024 data bytes of
 * sector i, then slot i's free OOB bytes, then its ECC), bits LSB first.
 *
 * nandecc DUMP OUT FIRSTBLOCK NBLOCKS
 *   OUT gets, per page, 8192 corrected data bytes + 96 OOB bytes (8 x 12).
 *   Erased pages stay 0xff.  stderr: a summary and the uncorrectable
 *   sectors.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M 14
#define N ((1 << M) - 1)
#define T 24
#define PRIM 0x402b
#define BLOCK 1104384
#define PAGE 8628
#define PAGES 128
#define CWB 1078                /* codeword bytes */
#define CWBITS (CWB * 8)        /* 8624 */
#define PBITS (M * T)           /* 336 parity bits */

static int exp_[2 * N], log_[N + 1];
static uint8_t gpoly[PBITS + 1];         /* generator coefficients */
static uint64_t tab[256][6];             /* remainder table, 336 bits MSB-first in 384 */

static int gmul(int a, int b) { return a && b ? exp_[log_[a] + log_[b]] : 0; }

static void init(void)
{
    int x = 1;
    for (int i = 0; i < N; i++) {
        exp_[i] = x;
        log_[x] = i;
        x <<= 1;
        if (x >> M) x ^= PRIM;
    }
    for (int i = N; i < 2 * N; i++) exp_[i] = exp_[i - N];
    /* g(x) = lcm of minimal polynomials of alpha^1..alpha^2T */
    int g[PBITS + 1] = { 1 }, dg = 0;    /* coefficients in GF(2^M) */
    static uint8_t done[N];
    for (int j = 1; j <= 2 * T; j++) {
        if (done[j]) continue;
        /* conjugates j, 2j, 4j, ... */
        int c = j;
        do {
            done[c] = 1;
            /* g *= (x - alpha^c) */
            int r = exp_[c];
            for (int k = dg + 1; k > 0; k--) g[k] = g[k - 1] ^ gmul(g[k], r);
            g[0] = gmul(g[0], r);
            dg++;
            c = (c * 2) % N;
        } while (c != j);
    }
    if (dg != PBITS) { fprintf(stderr, "generator degree %d\n", dg); exit(1); }
    for (int k = 0; k <= PBITS; k++) gpoly[k] = g[k];
    /* table: for byte b (MSB = highest degree), (b(x) * x^336) mod g */
    for (int b = 0; b < 256; b++) {
        uint8_t r[PBITS + 8] = { 0 };    /* r[k] = coeff of x^k */
        for (int i = 0; i < 8; i++) r[PBITS + i] = b >> i & 1;
        for (int d = PBITS + 7; d >= PBITS; d--)
            if (r[d])
                for (int k = 0; k <= PBITS; k++) r[d - PBITS + k] ^= gpoly[k];
        memset(tab[b], 0, sizeof(tab[b]));
        for (int k = 0; k < PBITS; k++)      /* x^335 -> bit 383 of the 384 */
            if (r[k]) {
                int pos = k + 48;            /* 384 - 336 */
                tab[b][5 - pos / 64] |= 1ull << (pos % 64);
            }
    }
}

static uint8_t rev8(uint8_t x)
{
    x = (x & 0xf0) >> 4 | (x & 0x0f) << 4;
    x = (x & 0xcc) >> 2 | (x & 0x33) << 2;
    return (x & 0xaa) >> 1 | (x & 0x55) << 1;
}

/*
 * Is the codeword a multiple of g?  r(x) <- r(x) x^8 + byte, mod g, one
 * byte at a time: r's 336 bits sit in the top of 6 words (word 0 highest),
 * x^0 at bit 48 of word 5; the byte shifted out goes through the table.
 */
static int bch_rem(const uint8_t *cw, uint64_t r[6])
{
    memset(r, 0, 48);
    for (int i = 0; i < CWB; i++) {
        uint8_t top = r[0] >> 56, in = rev8(cw[i]);
        for (int w = 0; w < 5; w++) r[w] = r[w] << 8 | r[w + 1] >> 56;
        r[5] <<= 8;
        r[5] |= (uint64_t)in << 48;
        for (int w = 0; w < 6; w++) r[w] ^= tab[top][w];
    }
    return (r[0] | r[1] | r[2] | r[3] | r[4] | r[5]) != 0;
}

/*
 * Fix a codeword with errors: syndromes from its bits (bit k in stream
 * order, LSB first, has degree CWBITS-1-k), Berlekamp-Massey, Chien
 * search.  Returns the bits corrected, 0 for none, -1 if uncorrectable.
 */
static int correct(uint8_t *cw)
{
    int S[2 * T + 1];
    for (int j = 1; j <= 2 * T; j++) S[j] = 0;
    for (int k = 0; k < CWBITS; k++)
        if (cw[k >> 3] >> (k & 7) & 1) {
            int d = CWBITS - 1 - k;
            for (int j = 1; j <= 2 * T; j += 2) S[j] ^= exp_[(j * d) % N];
        }
    for (int j = 2; j <= 2 * T; j += 2) S[j] = gmul(S[j / 2], S[j / 2]);
    int any = 0;
    for (int j = 1; j <= 2 * T; j++) any |= S[j];
    if (!any) return 0;
    /* Berlekamp-Massey */
    int C[2 * T + 2] = { 1 }, Bp[2 * T + 2] = { 1 }, L = 0, m = 1, b = 1;
    for (int n = 0; n < 2 * T; n++) {
        int d = S[n + 1];
        for (int i = 1; i <= L; i++) d ^= gmul(C[i], S[n + 1 - i]);
        if (!d) { m++; continue; }
        int Tt[2 * T + 2];
        memcpy(Tt, C, sizeof(C));
        int coef = gmul(d, exp_[(N - log_[b]) % N]);
        for (int i = 0; i + m < 2 * T + 2; i++) C[i + m] ^= gmul(coef, Bp[i]);
        if (2 * L <= n) {
            L = n + 1 - L;
            memcpy(Bp, Tt, sizeof(Tt));
            b = d;
            m = 1;
        } else
            m++;
    }
    if (L > T) return -1;
    /* Chien search: error at degree d if C(alpha^-d) = 0 */
    int found = 0, pos[T];
    for (int d = 0; d < CWBITS && found <= L; d++) {
        int v = 0;
        for (int i = 0; i <= L; i++)
            if (C[i]) v ^= exp_[(log_[C[i]] + (N - (i * d) % N)) % N];
        if (!v) {
            if (found == T) return -1;
            pos[found++] = d;
        }
    }
    if (found != L) return -1;
    for (int i = 0; i < found; i++) {
        int k = CWBITS - 1 - pos[i];
        cw[k >> 3] ^= 1 << (k & 7);
    }
    return found;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: nandecc DUMP OUT FIRSTBLOCK NBLOCKS\n");
        return 1;
    }
    init();
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    long first = atol(argv[3]), nb = atol(argv[4]);
    static uint8_t blk[BLOCK];
    long clean = 0, fixed = 0, bits = 0, bad = 0, noecc = 0, erased = 0;

    if (!in || !out) { perror("open"); return 1; }
    for (long bi = first; bi < first + nb; bi++) {
        fseek(in, bi * (long)BLOCK, SEEK_SET);
        if (fread(blk, 1, BLOCK, in) != BLOCK) break;
        for (int p = 0; p < PAGES; p++) {
            uint8_t *pg = blk + p * PAGE, *sp = pg + 8192, oob[96];
            int allff = 1;
            for (int i = 0; i < PAGE && allff; i++) allff = pg[i] == 0xff;
            if (allff) {
                erased++;
                memset(oob, 0xff, 96);
                fwrite(pg, 1, 8192, out);
                fwrite(oob, 1, 96, out);
                continue;
            }
            int eccff = 1;
            for (int s = 0; s < 8 && eccff; s++)
                for (int i = 0; i < 42; i++) eccff &= sp[54 * s + 12 + i] == 0xff;
            for (int s = 0; s < 8; s++) {
                uint8_t cw[CWB];
                uint64_t r[6];
                memcpy(cw, pg + 1024 * s, 1024);
                memcpy(cw + 1024, sp + 54 * s, 54);
                if (eccff) {
                    noecc++;
                } else if (!bch_rem(cw, r)) {
                    clean++;
                } else {
                    int n = correct(cw);
                    if (n > 0) { fixed++; bits += n; }
                    else if (n == 0) clean++;
                    else {
                        bad++;
                        fprintf(stderr, "uncorrectable: block %ld page %d sector %d\n", bi, p, s);
                    }
                }
                memcpy(pg + 1024 * s, cw, 1024);
                memcpy(oob + 12 * s, cw + 1024, 12);
            }
            fwrite(pg, 1, 8192, out);
            fwrite(oob, 1, 96, out);
        }
    }
    fprintf(stderr, "sectors: clean %ld, corrected %ld (%ld bits), uncorrectable %ld, "
            "no ECC %ld; erased pages %ld\n", clean, fixed, bits, bad, noecc, erased);
    fclose(out);
    return 0;
}
