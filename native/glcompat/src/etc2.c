/* DXT/BC -> ETC2/EAC transcoder (rev-3).
 *
 * Replaces the rev-2 "decode to RGBA8" fallback for drivers without
 * S3TC/RGTC: colour atlases stay block-compressed in GPU memory
 * (DXT1 -> ETC2 RGB8 at the same 4 bpp, DXT3/5 -> ETC2 RGBA8 at 16 bpp)
 * and single/dual-channel masks decode to R8/RG8 (8/16 bpp) instead of
 * RGBA8 (32 bpp).  On the Mali-G72 (SM-A515F) this cuts texture memory
 * from ~3 GB (RGBA8 decode of a 945 MB DXT working set, which exhausted
 * the ~1.3 GB GL budget and collapsed the game into unbatched per-sprite
 * rendering) back to roughly the game's own DXT estimate.
 *
 * Block layouts follow the reference decoder in detex (hglm/detex,
 * decompress-etc.c / decompress-eac.c), which this port treats as the
 * authority.  Big-endian 64-bit blocks, MSB = bit 63 of byte 0:
 *
 *   ETC1 colour block (differential mode only: in ETC2 the diff=0
 *   encoding is redefined as T/H modes, so this encoder never emits it):
 *     bits 63..56 R1(5) dR(3) sgn, 55..48 G1(5) dG(3), 47..40 B1(5) dB(3),
 *     bit 39 flip, bit 38 diff(=1), 37..35 table0, 34..32 table1,
 *     31..16 selector MSB plane (pixel i at bit 16+i),
 *     15..0  selector LSB plane (pixel i at bit i).
 *     Pixel i is COLUMN-major: i = x*4 + y (x = column, y = row).
 *
 *   EAC alpha (A8) block, first 8 bytes of ETC2 RGBA8:
 *     bits 63..56 base(8), 55..52 multiplier(4, encoded >= 1),
 *     51..48 table index(4), 47..0 16 selectors (pixel i at 45-3*i,
 *     column-major); value = clamp(base + modifier * multiplier).
 *
 *   RGBA8 = 8-byte EAC alpha block followed by 8-byte colour block.
 *
 * Modifier tables are verbatim from detex, which matches
 * rg_etc1/etcpak/basis_universal.
 */
#include "etc2.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Source format enums (local names; GL headers not required here)     */
/* ------------------------------------------------------------------ */

#define SRC_DXT1 0x83F0u
#define SRC_DXT1A 0x83F1u
#define SRC_DXT3 0x83F2u
#define SRC_DXT5 0x83F3u
#define SRC_BC4 0x8DBBu
#define SRC_BC5 0x8DBDu

/* ------------------------------------------------------------------ */
/* Tables (verbatim from detex decompress-etc.c / decompress-eac.c)    */
/* ------------------------------------------------------------------ */

/* ETC1 intensity modifier tables: [table][selector]. */
static const int kEtc1Mod[8][4] = {
    { 2, 8, -2, -8 },     { 5, 17, -5, -17 },   { 9, 29, -9, -29 },
    { 13, 42, -13, -42 }, { 18, 60, -18, -60 }, { 24, 80, -24, -80 },
    { 33, 106, -33, -106 },{ 47, 183, -47, -183 },
};

/* EAC modifier tables: [table][selector]. */
static const signed char kEacMod[16][8] = {
    { -3, -6, -9, -15, 2, 5, 8, 14 },
    { -3, -7, -10, -13, 2, 6, 9, 12 },
    { -2, -5, -8, -13, 1, 4, 7, 12 },
    { -2, -4, -6, -13, 1, 3, 5, 12 },
    { -3, -6, -8, -12, 2, 5, 7, 11 },
    { -3, -7, -9, -11, 2, 6, 8, 10 },
    { -4, -7, -8, -11, 3, 6, 7, 10 },
    { -3, -5, -8, -11, 2, 4, 7, 10 },
    { -2, -6, -8, -10, 1, 5, 7, 9 },
    { -2, -5, -8, -10, 1, 4, 7, 9 },
    { -2, -4, -8, -10, 1, 3, 7, 9 },
    { -2, -5, -7, -10, 1, 4, 6, 9 },
    { -3, -4, -7, -10, 2, 3, 6, 9 },
    { -1, -2, -3, -10, 0, 1, 2, 9 },
    { -4, -6, -8, -9, 3, 5, 7, 8 },
    { -3, -5, -7, -9, 2, 4, 6, 8 },
};

/* ------------------------------------------------------------------ */
/* DXT/BC block decode (16 RGBA pixels, row-major y*4+x)               */
/* ------------------------------------------------------------------ */

static void dec565(unsigned c, unsigned char *rgb)
{
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (unsigned char)((r << 3) | (r >> 2));
    rgb[1] = (unsigned char)((g << 2) | (g >> 4));
    rgb[2] = (unsigned char)((b << 3) | (b >> 2));
}

static void dec_color(const unsigned char *src, int force_four,
                      unsigned char out[16][4])
{
    unsigned c0 = (unsigned)src[0] | ((unsigned)src[1] << 8);
    unsigned c1 = (unsigned)src[2] | ((unsigned)src[3] << 8);
    unsigned char p[4][4];
    memset(p, 0, sizeof(p));
    dec565(c0, p[0]);
    dec565(c1, p[1]);
    p[0][3] = 255;
    p[1][3] = 255;
    if (force_four || c0 > c1) {
        for (int k = 0; k < 3; k++) {
            p[2][k] = (unsigned char)((2 * p[0][k] + p[1][k] + 1) / 3);
            p[3][k] = (unsigned char)((p[0][k] + 2 * p[1][k] + 1) / 3);
        }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) {
            p[2][k] = (unsigned char)((p[0][k] + p[1][k]) / 2);
        }
        p[2][3] = 255;
        p[3][0] = p[3][1] = p[3][2] = p[3][3] = 0;
    }
    unsigned bits = (unsigned)src[4] | ((unsigned)src[5] << 8) |
                    ((unsigned)src[6] << 16) | ((unsigned)src[7] << 24);
    for (int i = 0; i < 16; i++) {
        unsigned idx = (bits >> (2 * i)) & 3u;
        memcpy(out[i], p[idx], 4);
    }
}

static void dec_dxt1(const unsigned char *src, unsigned char out[16][4])
{
    dec_color(src, 0, out);
}

static void dec_dxt3(const unsigned char *src, unsigned char out[16][4])
{
    dec_color(src + 8, 1, out);
    for (int i = 0; i < 16; i++) {
        unsigned a = ((unsigned)src[i / 2] >> ((i & 1) * 4)) & 0xFu;
        out[i][3] = (unsigned char)((a << 4) | a);
    }
}

static void rgtc_values(const unsigned char *src, unsigned char v[16])
{
    unsigned char t[8];
    t[0] = src[0];
    t[1] = src[1];
    if (t[0] > t[1]) {
        for (int i = 1; i <= 6; i++) {
            t[i + 1] = (unsigned char)(((7 - i) * t[0] + i * t[1] + 3) / 7);
        }
    } else {
        for (int i = 1; i <= 4; i++) {
            t[i + 1] = (unsigned char)(((5 - i) * t[0] + i * t[1] + 2) / 5);
        }
        t[6] = 0;
        t[7] = 255;
    }
    unsigned long long bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= (unsigned long long)src[2 + i] << (8 * i);
    }
    for (int i = 0; i < 16; i++) {
        v[i] = t[(bits >> (3 * i)) & 7u];
    }
}

static void dec_dxt5_alpha(const unsigned char *src, unsigned char out[16][4])
{
    /* Build the 8-entry alpha ramp (same interpolation as BC4/BC5). */
    unsigned char a[8];
    a[0] = src[0];
    a[1] = src[1];
    if (a[0] > a[1]) {
        for (int i = 1; i <= 6; i++) {
            a[i + 1] = (unsigned char)(((7 - i) * a[0] + i * a[1] + 3) / 7);
        }
    } else {
        for (int i = 1; i <= 4; i++) {
            a[i + 1] = (unsigned char)(((5 - i) * a[0] + i * a[1] + 2) / 5);
        }
        a[6] = 0;
        a[7] = 255;
    }
    unsigned long long bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= (unsigned long long)src[2 + i] << (8 * i);
    }
    for (int i = 0; i < 16; i++) {
        out[i][3] = a[(bits >> (3 * i)) & 7u];
    }
}

static void dec_dxt5(const unsigned char *src, unsigned char out[16][4])
{
    dec_color(src + 8, 1, out);
    dec_dxt5_alpha(src, out);
}

static void dec_bc4(const unsigned char *src, unsigned char out[16][4])
{
    unsigned char v[16];
    rgtc_values(src, v);
    for (int i = 0; i < 16; i++) {
        out[i][0] = v[i];
        out[i][1] = out[i][2] = 0;
        out[i][3] = 255;
    }
}

static void dec_bc5(const unsigned char *src, unsigned char out[16][4])
{
    unsigned char vr[16], vg[16];
    rgtc_values(src, vr);
    rgtc_values(src + 8, vg);
    for (int i = 0; i < 16; i++) {
        out[i][0] = vr[i];
        out[i][1] = vg[i];
        out[i][2] = 0;
        out[i][3] = 255;
    }
}

/* ------------------------------------------------------------------ */
/* ETC1 differential colour encode                                     */
/* ------------------------------------------------------------------ */

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int expand4(int v)
{
    return (v << 4) | v;
}

static int expand5(int v)
{
    return (v << 3) | (v >> 2);
}

/* Exact codeword fields + expanded endpoint colours for one candidate. */
typedef struct {
    int diff;       /* 1 = differential mode, 0 = individual mode */
    int b5[3];      /* differential: subblock 0 base codewords (5 bit) */
    int d3[3];      /* differential: subblock 1 deltas biased +4 (0..7) */
    int nib[2][3];  /* individual: 4-bit nibbles, [subblock][channel] */
    int base[2][3]; /* expanded 8-bit endpoints per subblock */
} EtcFields;

static void fields_differential(const int mean[2][3], EtcFields *f)
{
    f->diff = 1;
    for (int c = 0; c < 3; c++) {
        int b0 = clampi((mean[0][c] * 31 + 127) / 255, 0, 31);
        int want = clampi((mean[1][c] * 31 + 127) / 255, 0, 31);
        int d = clampi(want - b0, -4, 3);
        f->b5[c] = b0;
        f->d3[c] = d + 4;
        f->base[0][c] = expand5(b0);
        f->base[1][c] = expand5(clampi(b0 + d, 0, 31));
    }
}

static void fields_individual(const int mean[2][3], EtcFields *f)
{
    f->diff = 0;
    for (int sb = 0; sb < 2; sb++) {
        for (int c = 0; c < 3; c++) {
            int v = clampi((mean[sb][c] * 15 + 127) / 255, 0, 15);
            f->nib[sb][c] = v;
            f->base[sb][c] = expand4(v);
        }
    }
}

/* Modifier-table search for one subblock; writes selectors into sel16 at
 * visual position y*4+x.  Only the three tables whose large modifier
 * best matches three times the mean deviation are searched (DCT-style
 * ramps reach their extremes at ~3x the mean step; flat blocks use the
 * smallest table).  Returns the achieved SSE. */
static long subblock_encode(const unsigned char px[16][4], int flip,
                            int sub, const int base_rgb[3],
                            unsigned *out_table, unsigned char sel16[16])
{
    long dev_sum = 0;
    int npx = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int sb = flip ? (y >> 1) : (x >> 1);
            if (sb != sub) {
                continue;
            }
            const unsigned char *p = px[y * 4 + x];
            dev_sum += (labs(base_rgb[0] - p[0]) + labs(base_rgb[1] - p[1]) +
                        labs(base_rgb[2] - p[2])) / 3;
            npx++;
        }
    }
    int dev = npx ? (int)(dev_sum / npx) : 0;

    static const int big[8] = { 8, 17, 29, 42, 60, 80, 106, 183 };
    /* A symmetric 4-level DCT ramp {base±s, base±3s} has mean deviation
     * 2s, so the large modifier wants to be ~1.5x the mean deviation. */
    int want = dev * 3 / 2;
    int near = 0;
    for (int t = 1; t < 8; t++) {
        if (labs(big[t] - want) < labs(big[near] - want)) {
            near = t;
        }
    }
    int cand[3] = { near > 0 ? near - 1 : 0, near, near < 7 ? near + 1 : 7 };

    long best_sse = -1;
    unsigned char tmp[16];
    for (int ci = 0; ci < 3; ci++) {
        int cw = cand[ci];
        const int *mod = kEtc1Mod[cw];
        /* Sort this table's modifiers ascending, keeping detex selectors.
         * The per-pixel objective f(mod) = 3*mod^2 + 2*L*mod + C (with
         * L = sum_c (base_c - p_c)) is a parabola, so the best modifier
         * is the one closest to -L/3; the boundaries are pixel-
         * independent: L <= -3*(m_i + m_i+1)/2 <=> sorted index <= i. */
        int m[4], sel_of[4];
        for (int k = 0; k < 4; k++) {
            m[k] = mod[k];
            sel_of[k] = k;
        }
        for (int a = 1; a < 4; a++) {
            for (int b = a; b > 0 && m[b] < m[b - 1]; b--) {
                int tm = m[b]; m[b] = m[b - 1]; m[b - 1] = tm;
                int ts = sel_of[b]; sel_of[b] = sel_of[b - 1]; sel_of[b - 1] = ts;
            }
        }
        int thr[3];
        for (int i = 0; i < 3; i++) {
            thr[i] = -3 * (m[i] + m[i + 1]) / 2;
        }
        long sse = 0;
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int sb = flip ? (y >> 1) : (x >> 1);
                if (sb != sub) {
                    continue;
                }
                const unsigned char *p = px[y * 4 + x];
                int L = (base_rgb[0] - p[0]) + (base_rgb[1] - p[1]) +
                        (base_rgb[2] - p[2]);
                int idx = 0;
                if (L <= thr[0]) idx = 1;
                if (L <= thr[1]) idx = 2;
                if (L <= thr[2]) idx = 3;
                int mv = m[idx];
                int dr = base_rgb[0] + mv - p[0];
                int dg = base_rgb[1] + mv - p[1];
                int db = base_rgb[2] + mv - p[2];
                sse += (long)dr * dr + (long)dg * dg + (long)db * db;
                tmp[y * 4 + x] = (unsigned char)sel_of[idx];
            }
        }
        if (best_sse < 0 || sse < best_sse) {
            best_sse = sse;
            *out_table = (unsigned)cw;
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int sb = flip ? (y >> 1) : (x >> 1);
                    if (sb == sub) {
                        sel16[y * 4 + x] = tmp[y * 4 + x];
                    }
                }
            }
        }
    }
    return best_sse;
}

/* Encode one 4x4 RGBA block.  Differential mode is always evaluated;
 * individual mode only when the differential delta clamped (its 4-bit
 * endpoints are otherwise strictly coarser).  In ETC2 the individual
 * mode (diff=0) keeps its ETC1 meaning; T/H/Planar are only reached by
 * overflow inside the differential branch, which this encoder never
 * emits. */
static uint64_t etc1_encode_block(const unsigned char px[16][4])
{
    long best_total = -1;
    int best_flip = 0;
    EtcFields best_f;
    memset(&best_f, 0, sizeof(best_f));
    unsigned best_tab[2] = { 0, 0 };
    unsigned char best_sel[16] = { 0 };

    for (int flip = 0; flip <= 1; flip++) {
        long sum[2][3] = { { 0 } };
        int cnt[2] = { 0, 0 };
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int sb = flip ? (y >> 1) : (x >> 1);
                const unsigned char *p = px[y * 4 + x];
                for (int c = 0; c < 3; c++) {
                    sum[sb][c] += p[c];
                }
                cnt[sb]++;
            }
        }
        int mean[2][3];
        for (int sb = 0; sb < 2; sb++) {
            for (int c = 0; c < 3; c++) {
                mean[sb][c] = cnt[sb] ? (int)(sum[sb][c] / cnt[sb]) : 0;
            }
        }
        int clamped = 0;
        for (int c = 0; c < 3; c++) {
            int b0 = clampi((mean[0][c] * 31 + 127) / 255, 0, 31);
            int want = clampi((mean[1][c] * 31 + 127) / 255, 0, 31);
            if (want - b0 < -4 || want - b0 > 3) {
                clamped = 1;
                break;
            }
        }
        for (int mode = clamped ? 0 : 1; mode < 2; mode++) {
            EtcFields f;
            if (mode == 0) {
                fields_differential(mean, &f);
            } else {
                fields_individual(mean, &f);
            }
            long total = 0;
            unsigned tab[2];
            unsigned char sel[16];
            memcpy(sel, best_sel, sizeof(sel));
            for (int sb = 0; sb < 2; sb++) {
                total += subblock_encode(px, flip, sb, f.base[sb], &tab[sb], sel);
            }
            if (best_total < 0 || total < best_total) {
                best_total = total;
                best_flip = flip;
                best_f = f;
                best_tab[0] = tab[0];
                best_tab[1] = tab[1];
                memcpy(best_sel, sel, sizeof(best_sel));
            }
        }
    }

    uint64_t d = 0;
    if (best_f.diff) {
        for (int c = 0; c < 3; c++) {
            d |= (uint64_t)(best_f.b5[c] & 0x1F) << (59 - 8 * c);
            d |= (uint64_t)(best_f.d3[c] & 7) << (56 - 8 * c);
        }
    } else {
        /* Individual mode nibbles: byte0 = R1(hi) R2(lo), byte1 = G1 G2,
         * byte2 = B1 B2 (detex decompress-etc.c), i.e. R1 at bits 63..60,
         * R2 59..56, G1 55..52, G2 51..48, B1 47..44, B2 43..40. */
        for (int sb = 0; sb < 2; sb++) {
            for (int c = 0; c < 3; c++) {
                d |= (uint64_t)(best_f.nib[sb][c] & 0xF) << (60 - (sb + c * 2) * 4);
            }
        }
    }
    d |= (uint64_t)(best_flip & 1) << 39;
    d |= (uint64_t)(best_f.diff & 1) << 38;
    d |= (uint64_t)(best_tab[0] & 7) << 35;
    d |= (uint64_t)(best_tab[1] & 7) << 32;
    /* Selectors: pixel i is column-major (i = x*4 + y); LSB plane bits
     * 15..0 (pixel i at bit i), MSB plane bits 31..16 (bit 16+i). */
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 4; y++) {
            int i = x * 4 + y;
            unsigned s = best_sel[y * 4 + x];
            d |= (uint64_t)((s >> 1) & 1) << (16 + i);
            d |= (uint64_t)(s & 1) << i;
        }
    }
    return d;
}

/* ------------------------------------------------------------------ */
/* EAC alpha encode                                                    */
/* ------------------------------------------------------------------ */

static uint64_t eac_encode_alpha(const unsigned char px[16][4])
{
    int amin = 255, amax = 0, pmin = 0, pmax = 0;
    long sum = 0;
    for (int i = 0; i < 16; i++) {
        int a = px[i][3];
        sum += a;
        if (a < amin) { amin = a; pmin = i; }
        if (a > amax) { amax = a; pmax = i; }
    }
    int base = clampi((int)(sum / 16), 0, 255);

    if (amin == amax) {
        /* Flat block encodes exactly. */
        return ((uint64_t)(unsigned)base << 56) | ((uint64_t)1 << 52);
    }

    /* Candidate multipliers from the block's alpha span: the widest
     * modifier span is ~29 (tables reach +-15, span 23..29), so the
     * smallest covering multiplier is ~span/29; try it and its
     * neighbours.  Probe the 16 tables on the two extreme pixels and
     * fully evaluate the best three (multiplier, table) combos. */
    int span = amax - amin;
    int probe[2] = { pmin, pmax };
    int mc = clampi(span / 28, 1, 15);
    int m_cand[3] = { mc > 1 ? mc - 1 : 1, mc, mc < 15 ? mc + 1 : 15 };
    int best_m[3], best_t[3];
    long best_e[3] = { -1, -1, -1 };
    int n_m = (m_cand[0] != m_cand[1] && m_cand[1] != m_cand[2] &&
               m_cand[0] != m_cand[2]) ? 3 : (m_cand[0] != m_cand[1] ? 2 : 1);
    for (int mi = 0; mi < n_m; mi++) {
        int m = m_cand[mi];
        for (int t = 0; t < 16; t++) {
            long e = 0;
            for (int p = 0; p < 2; p++) {
                int a = px[probe[p]][3];
                int best = 1 << 30;
                for (int k = 0; k < 8; k++) {
                    int v = clampi(base + kEacMod[t][k] * m, 0, 255);
                    int d = v - a;
                    if (d < 0) d = -d;
                    if (d < best) best = d;
                }
                e += best;
            }
            for (int slot = 0; slot < 3; slot++) {
                if (best_e[slot] < 0 || e < best_e[slot]) {
                    for (int s2 = 2; s2 > slot; s2--) {
                        best_e[s2] = best_e[s2 - 1];
                        best_m[s2] = best_m[s2 - 1];
                        best_t[s2] = best_t[s2 - 1];
                    }
                    best_e[slot] = e;
                    best_m[slot] = m;
                    best_t[slot] = t;
                    break;
                }
            }
        }
    }

    long best_sse = -1;
    int use_m = 1, use_t = 0;
    unsigned char sel16[16] = { 0 };
    for (int slot = 0; slot < 3; slot++) {
        if (best_e[slot] < 0) {
            continue;
        }
        int m = best_m[slot], t = best_t[slot];
        /* Sort this combo's scaled modifiers ascending with selectors;
         * f(s) = (s - target)^2 with target = a - base is minimised by
         * the s closest to the target, so midpoint boundaries give the
         * exact argmin without an 8-way loop per pixel. */
        int s[8], sel_of[8];
        for (int k = 0; k < 8; k++) {
            s[k] = kEacMod[t][k] * m;
            sel_of[k] = k;
        }
        for (int a = 1; a < 8; a++) {
            for (int b = a; b > 0 && s[b] < s[b - 1]; b--) {
                int tv = s[b]; s[b] = s[b - 1]; s[b - 1] = tv;
                int ts = sel_of[b]; sel_of[b] = sel_of[b - 1]; sel_of[b - 1] = ts;
            }
        }
        int mid[7];
        for (int i = 0; i < 7; i++) {
            mid[i] = (s[i] + s[i + 1]) / 2;
        }
        long sse = 0;
        unsigned char sel[16];
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int target = (int)px[y * 4 + x][3] - base;
                int idx = 0;
                for (int i = 0; i < 7; i++) {
                    if (target >= mid[i]) {
                        idx = i + 1;
                    } else {
                        break;
                    }
                }
                int v = clampi(base + s[idx], 0, 255);
                long e = (long)(v - (int)px[y * 4 + x][3]) * (v - (int)px[y * 4 + x][3]);
                sel[y * 4 + x] = (unsigned char)sel_of[idx];
                sse += e;
            }
        }
        if (best_sse < 0 || sse < best_sse) {
            best_sse = sse;
            use_m = m;
            use_t = t;
            memcpy(sel16, sel, 16);
        }
    }

    uint64_t d = (uint64_t)(unsigned)base << 56;
    d |= (uint64_t)(use_m & 0xF) << 52;
    d |= (uint64_t)(use_t & 0xF) << 48;
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 4; y++) {
            int i = x * 4 + y;
            d |= (uint64_t)(sel16[y * 4 + x] & 7) << (45 - 3 * i);
        }
    }
    return d;
}

/* ------------------------------------------------------------------ */
/* ETC2/EAC decode (readback support; matches the encoder's modes)     */
/* ------------------------------------------------------------------ */

static int etc2_dclamp(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* Decode one ETC1-mode colour block (differential or individual) into
 * 16 RGBA pixels, row-major y*4+x.  keep_alpha leaves the alpha channel
 * untouched (used after etc2_decode_alpha); otherwise alpha is set to 255. */
static void etc2_decode_color(uint64_t d, unsigned char out[16][4], int keep_alpha)
{
    int flip = (int)((d >> 39) & 1);
    int cw0 = (int)((d >> 35) & 7);
    int cw1 = (int)((d >> 32) & 7);
    int base[2][3];
    if ((d >> 38) & 1) {
        for (int c = 0; c < 3; c++) {
            int b5 = (int)((d >> (59 - 8 * c)) & 0x1F);
            int d3 = (int)((d >> (56 - 8 * c)) & 7) - 4;
            base[0][c] = expand5(b5);
            int v = b5 + d3;
            v = v < 0 ? 0 : (v > 31 ? 31 : v);
            base[1][c] = expand5(v);
        }
    } else {
        for (int sb = 0; sb < 2; sb++) {
            for (int c = 0; c < 3; c++) {
                int n4 = (int)((d >> (60 - (sb + c * 2) * 4)) & 0xF);
                base[sb][c] = (n4 << 4) | n4;
            }
        }
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int i = x * 4 + y; /* column-major pixel order */
            int sb = flip ? (y >> 1) : (x >> 1);
            int sel = (int)(((d >> (16 + i)) & 1) << 1) | (int)((d >> i) & 1);
            int mod = kEtc1Mod[sb ? cw1 : cw0][sel];
            out[y * 4 + x][0] = (unsigned char)etc2_dclamp(base[sb][0] + mod);
            out[y * 4 + x][1] = (unsigned char)etc2_dclamp(base[sb][1] + mod);
            out[y * 4 + x][2] = (unsigned char)etc2_dclamp(base[sb][2] + mod);
            if (!keep_alpha) {
                out[y * 4 + x][3] = 255;
            }
        }
    }
}

/* Decode one EAC alpha block over existing RGBA pixels. */
static void etc2_decode_alpha(uint64_t d, unsigned char out[16][4])
{
    int base = (int)((d >> 56) & 0xFF);
    int mult = (int)((d >> 52) & 0xF);
    int table = (int)((d >> 48) & 0xF);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int i = x * 4 + y;
            int sel = (int)((d >> (45 - 3 * i)) & 7);
            out[y * 4 + x][3] =
                (unsigned char)etc2_dclamp(base + kEacMod[table][sel] * mult);
        }
    }
}

static uint64_t get64be(const unsigned char *src)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)src[i] << (56 - 8 * i);
    }
    return v;
}

static void put64(unsigned char *dst, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        dst[i] = (unsigned char)(v >> (56 - 8 * i));
    }
}

int fsgl_etc2_decode_rgba(uint32_t etc2_fmt, int w, int h, const void *data,
                          size_t size, unsigned char *out_rgba)
{    if (!data || !out_rgba || w <= 0 || h <= 0) {
        return -1;
    }
    unsigned block_bytes;
    if (etc2_fmt == FSGL_ETC2_RGB8) {
        block_bytes = 8;
    } else if (etc2_fmt == FSGL_ETC2_RGBA8) {
        block_bytes = 16;
    } else {
        return -1;
    }
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    if (size < (size_t)bw * (size_t)bh * (size_t)block_bytes) {
        return -1;
    }
    const unsigned char *src = (const unsigned char *)data;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            unsigned char px[16][4];
            const unsigned char *bs = src + ((size_t)by * bw + bx) * (size_t)block_bytes;
            if (block_bytes == 16) {
                etc2_decode_alpha(get64be(bs), px);
                etc2_decode_color(get64be(bs + 8), px, 1);
            } else {
                etc2_decode_color(get64be(bs), px, 0);
            }
            for (int y = 0; y < 4; y++) {
                int py = by * 4 + y;
                if (py >= h) {
                    break;
                }
                for (int x = 0; x < 4; x++) {
                    int pxx = bx * 4 + x;
                    if (pxx >= w) {
                        break;
                    }
                    memcpy(out_rgba + ((size_t)py * (size_t)w + (size_t)pxx) * 4u,
                           px[y * 4 + x], 4);
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

uint32_t fsgl_etc2_dest_format(uint32_t dxt_fmt)
{
    switch (dxt_fmt) {
    case SRC_DXT1:
        return FSGL_ETC2_RGB8;
    case SRC_DXT3:
    case SRC_DXT5:
        return FSGL_ETC2_RGBA8;
    case SRC_BC4:
        return FSGL_R8;
    case SRC_BC5:
        return FSGL_RG8;
    default:
        return 0;
    }
}

unsigned fsgl_etc2_dest_block_bytes(uint32_t dest_fmt)
{
    switch (dest_fmt) {
    case FSGL_ETC2_RGB8:
        return 8;
    case FSGL_ETC2_RGBA8:
        return 16;
    default:
        return 0;
    }
}

size_t fsgl_etc2_dest_size(uint32_t dest_fmt, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return 0;
    }
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    switch (dest_fmt) {
    case FSGL_ETC2_RGB8:
        return (size_t)bw * (size_t)bh * 8u;
    case FSGL_ETC2_RGBA8:
        return (size_t)bw * (size_t)bh * 16u;
    case FSGL_R8:
        return (size_t)w * (size_t)h;
    case FSGL_RG8:
        return (size_t)w * (size_t)h * 2u;
    default:
        return 0;
    }
}

unsigned char *fsgl_transcode_to_etc2(uint32_t dxt_fmt, int w, int h,
                                      const void *data, size_t image_size,
                                      uint32_t *out_fmt, size_t *out_size)
{
    if (!data || w <= 0 || h <= 0) {
        return NULL;
    }
    uint32_t dest = fsgl_etc2_dest_format(dxt_fmt);
    if (!dest) {
        return NULL;
    }
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    int src_block_bytes;
    switch (dxt_fmt) {
    case SRC_DXT1:
    case SRC_BC4:
        src_block_bytes = 8;
        break;
    case SRC_DXT3:
    case SRC_DXT5:
    case SRC_BC5:
        src_block_bytes = 16;
        break;
    default:
        return NULL;
    }
    if (image_size > 0 && image_size < (size_t)bw * (size_t)bh * (size_t)src_block_bytes) {
        return NULL;
    }
    size_t out_sz = fsgl_etc2_dest_size(dest, w, h);
    if (!out_sz) {
        return NULL;
    }
    unsigned char *out = (unsigned char *)malloc(out_sz);
    if (!out) {
        return NULL;
    }
    const unsigned char *src = (const unsigned char *)data;

    if (dest == FSGL_R8 || dest == FSGL_RG8) {
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                unsigned char px[16][4];
                if (dxt_fmt == SRC_BC4) {
                    dec_bc4(src + ((size_t)by * bw + bx) * 8u, px);
                } else {
                    dec_bc5(src + ((size_t)by * bw + bx) * 16u, px);
                }
                for (int y = 0; y < 4; y++) {
                    int py = by * 4 + y;
                    if (py >= h) {
                        break;
                    }
                    for (int x = 0; x < 4; x++) {
                        int px_ = bx * 4 + x;
                        if (px_ >= w) {
                            break;
                        }
                        size_t di = (size_t)py * (size_t)w + (size_t)px_;
                        if (dest == FSGL_R8) {
                            out[di] = px[y * 4 + x][0];
                        } else {
                            out[di * 2] = px[y * 4 + x][0];
                            out[di * 2 + 1] = px[y * 4 + x][1];
                        }
                    }
                }
            }
        }
    } else {
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                unsigned char px[16][4];
                const unsigned char *bs = src + ((size_t)by * bw + bx) * (size_t)src_block_bytes;
                if (dxt_fmt == SRC_DXT1) {
                    dec_dxt1(bs, px);
                } else if (dxt_fmt == SRC_DXT3) {
                    dec_dxt3(bs, px);
                } else {
                    dec_dxt5(bs, px);
                }
                unsigned char *bd = out + ((size_t)by * bw + bx) *
                                     (dest == FSGL_ETC2_RGBA8 ? 16u : 8u);
                if (dest == FSGL_ETC2_RGBA8) {
                    put64(bd, eac_encode_alpha(px));
                    put64(bd + 8, etc1_encode_block(px));
                } else {
                    put64(bd, etc1_encode_block(px));
                }
            }
        }
    }
    if (out_fmt) {
        *out_fmt = dest;
    }
    if (out_size) {
        *out_size = out_sz;
    }
    return out;
}

int fsgl_etc2_encode_rgba(uint32_t dest_fmt, int w, int h, const void *rgba,
                          unsigned char *out)
{
    if (!rgba || !out || w <= 0 || h <= 0) {
        return -1;
    }
    unsigned block_bytes;
    if (dest_fmt == FSGL_ETC2_RGB8) {
        block_bytes = 8;
    } else if (dest_fmt == FSGL_ETC2_RGBA8) {
        block_bytes = 16;
    } else {
        return -1;
    }
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    const unsigned char *src = (const unsigned char *)rgba;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            unsigned char px[16][4];
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int sy = by * 4 + y;
                    int sx = bx * 4 + x;
                    unsigned char *d = px[y * 4 + x];
                    if (sy < h && sx < w) {
                        const unsigned char *s =
                            src + ((size_t)sy * (size_t)w + (size_t)sx) * 4u;
                        memcpy(d, s, 4);
                    } else {
                        d[0] = d[1] = d[2] = d[3] = 0;
                    }
                }
            }
            unsigned char *bd = out + ((size_t)by * bw + bx) * (size_t)block_bytes;
            if (block_bytes == 16) {
                put64(bd, eac_encode_alpha(px));
                put64(bd + 8, etc1_encode_block(px));
            } else {
                put64(bd, etc1_encode_block(px));
            }
        }
    }
    return 0;
}

void fsgl_etc2_box_filter_rgba(const unsigned char *src, int sw, int sh,
                               unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            long r = 0, g = 0, b = 0, asum = 0, wsum = 0;
            int n = 0;
            for (int dy = 0; dy < 2; dy++) {
                int sy = y * 2 + dy;
                if (sy >= sh) {
                    continue;
                }
                for (int dx = 0; dx < 2; dx++) {
                    int sx = x * 2 + dx;
                    if (sx >= sw) {
                        continue;
                    }
                    const unsigned char *p = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4u;
                    long w = (long)p[3] + 1; /* alpha-weighted (straight alpha) */
                    r += (long)p[0] * w;
                    g += (long)p[1] * w;
                    b += (long)p[2] * w;
                    asum += p[3];
                    wsum += w;
                    n++;
                }
            }
            unsigned char *o = dst + ((size_t)y * (size_t)dw + (size_t)x) * 4u;
            if (wsum > 0 && n > 0) {
                o[0] = (unsigned char)(r / wsum);
                o[1] = (unsigned char)(g / wsum);
                o[2] = (unsigned char)(b / wsum);
                o[3] = (unsigned char)(asum / n);
            } else {
                o[0] = o[1] = o[2] = o[3] = 0;
            }
        }
    }
}
