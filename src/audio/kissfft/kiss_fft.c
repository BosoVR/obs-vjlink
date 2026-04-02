/*
 * KissFFT - A mixed-radix Fast Fourier Transform
 * Copyright (c) 2003-2010 Mark Borgerding
 * All rights reserved.
 * BSD License - see kiss_fft.h
 */
#include "kiss_fft.h"
#include <limits.h>

#define MAXFACTORS 32

struct kiss_fft_state {
    int nfft;
    int inverse;
    int factors[2 * MAXFACTORS];
    kiss_fft_cpx twiddles[1]; /* flexible array */
};

/* Twiddle factor computation */
static void kf_cexp(kiss_fft_cpx *x, double phase)
{
    x->r = (kiss_fft_scalar)cos(phase);
    x->i = (kiss_fft_scalar)sin(phase);
}

#define C_ADD(res, a, b) \
    do { (res).r = (a).r + (b).r; (res).i = (a).i + (b).i; } while(0)
#define C_SUB(res, a, b) \
    do { (res).r = (a).r - (b).r; (res).i = (a).i - (b).i; } while(0)
#define C_MUL(m, a, b) \
    do { (m).r = (a).r*(b).r - (a).i*(b).i; \
         (m).i = (a).r*(b).i + (a).i*(b).r; } while(0)
#define C_MULBYSCALAR(c, s) \
    do { (c).r *= (s); (c).i *= (s); } while(0)
#define C_ADDTO(res, a) \
    do { (res).r += (a).r; (res).i += (a).i; } while(0)

static void kf_bfly2(kiss_fft_cpx *Fout, const size_t fstride,
                     const kiss_fft_cfg st, int m)
{
    kiss_fft_cpx *Fout2;
    kiss_fft_cpx *tw1 = st->twiddles;
    kiss_fft_cpx t;
    Fout2 = Fout + m;
    do {
        C_MUL(t, *Fout2, *tw1);
        tw1 += fstride;
        C_SUB(*Fout2, *Fout, t);
        C_ADD(*Fout, *Fout, t);
        ++Fout2;
        ++Fout;
    } while (--m);
}

static void kf_bfly4(kiss_fft_cpx *Fout, const size_t fstride,
                     const kiss_fft_cfg st, const size_t m)
{
    kiss_fft_cpx *tw1, *tw2, *tw3;
    kiss_fft_cpx scratch[6];
    size_t k = m;
    const size_t m2 = 2 * m;
    const size_t m3 = 3 * m;

    tw3 = tw2 = tw1 = st->twiddles;

    do {
        C_MUL(scratch[0], Fout[m], *tw1);
        C_MUL(scratch[1], Fout[m2], *tw2);
        C_MUL(scratch[2], Fout[m3], *tw3);

        C_SUB(scratch[5], *Fout, scratch[1]);
        C_ADD(*Fout, *Fout, scratch[1]);
        C_ADD(scratch[3], scratch[0], scratch[2]);
        C_SUB(scratch[4], scratch[0], scratch[2]);
        C_SUB(Fout[m2], *Fout, scratch[3]);
        tw1 += fstride;
        tw2 += fstride * 2;
        tw3 += fstride * 3;
        C_ADD(*Fout, *Fout, scratch[3]);

        if (st->inverse) {
            Fout[m].r = scratch[5].r - scratch[4].i;
            Fout[m].i = scratch[5].i + scratch[4].r;
            Fout[m3].r = scratch[5].r + scratch[4].i;
            Fout[m3].i = scratch[5].i - scratch[4].r;
        } else {
            Fout[m].r = scratch[5].r + scratch[4].i;
            Fout[m].i = scratch[5].i - scratch[4].r;
            Fout[m3].r = scratch[5].r - scratch[4].i;
            Fout[m3].i = scratch[5].i + scratch[4].r;
        }
        ++Fout;
    } while (--k);
}

static void kf_bfly3(kiss_fft_cpx *Fout, const size_t fstride,
                     const kiss_fft_cfg st, size_t m)
{
    size_t k = m;
    const size_t m2 = 2 * m;
    kiss_fft_cpx *tw1, *tw2;
    kiss_fft_cpx scratch[5];
    kiss_fft_cpx epi3;
    epi3 = st->twiddles[fstride * m];

    tw1 = tw2 = st->twiddles;

    do {
        C_MUL(scratch[1], Fout[m], *tw1);
        C_MUL(scratch[2], Fout[m2], *tw2);

        C_ADD(scratch[3], scratch[1], scratch[2]);
        C_SUB(scratch[0], scratch[1], scratch[2]);
        tw1 += fstride;
        tw2 += fstride * 2;

        Fout[m].r = Fout->r - scratch[3].r * 0.5f;
        Fout[m].i = Fout->i - scratch[3].i * 0.5f;

        C_MULBYSCALAR(scratch[0], epi3.i);

        C_ADDTO(*Fout, scratch[3]);

        Fout[m2].r = Fout[m].r + scratch[0].i;
        Fout[m2].i = Fout[m].i - scratch[0].r;

        Fout[m].r -= scratch[0].i;
        Fout[m].i += scratch[0].r;

        ++Fout;
    } while (--k);
}

static void kf_bfly5(kiss_fft_cpx *Fout, const size_t fstride,
                     const kiss_fft_cfg st, int m)
{
    kiss_fft_cpx *Fout0, *Fout1, *Fout2, *Fout3, *Fout4;
    int u;
    kiss_fft_cpx scratch[13];
    kiss_fft_cpx *twiddles = st->twiddles;
    kiss_fft_cpx ya, yb;
    ya = twiddles[fstride * m];
    yb = twiddles[fstride * 2 * m];

    Fout0 = Fout;
    Fout1 = Fout0 + m;
    Fout2 = Fout0 + 2 * m;
    Fout3 = Fout0 + 3 * m;
    Fout4 = Fout0 + 4 * m;

    for (u = 0; u < m; ++u) {
        scratch[0] = *Fout0;

        C_MUL(scratch[1], *Fout1, twiddles[u * fstride]);
        C_MUL(scratch[2], *Fout2, twiddles[2 * u * fstride]);
        C_MUL(scratch[3], *Fout3, twiddles[3 * u * fstride]);
        C_MUL(scratch[4], *Fout4, twiddles[4 * u * fstride]);

        C_ADD(scratch[7], scratch[1], scratch[4]);
        C_SUB(scratch[10], scratch[1], scratch[4]);
        C_ADD(scratch[8], scratch[2], scratch[3]);
        C_SUB(scratch[9], scratch[2], scratch[3]);

        Fout0->r += scratch[7].r + scratch[8].r;
        Fout0->i += scratch[7].i + scratch[8].i;

        scratch[5].r = scratch[0].r + scratch[7].r * ya.r + scratch[8].r * yb.r;
        scratch[5].i = scratch[0].i + scratch[7].i * ya.r + scratch[8].i * yb.r;

        scratch[6].r = scratch[10].i * ya.i + scratch[9].i * yb.i;
        scratch[6].i = -(scratch[10].r * ya.i + scratch[9].r * yb.i);

        C_SUB(*Fout1, scratch[5], scratch[6]);
        C_ADD(*Fout4, scratch[5], scratch[6]);

        scratch[11].r = scratch[0].r + scratch[7].r * yb.r + scratch[8].r * ya.r;
        scratch[11].i = scratch[0].i + scratch[7].i * yb.r + scratch[8].i * ya.r;

        scratch[12].r = -(scratch[10].i * yb.i) + scratch[9].i * ya.i;
        scratch[12].i = scratch[10].r * yb.i - scratch[9].r * ya.i;

        C_ADD(*Fout2, scratch[11], scratch[12]);
        C_SUB(*Fout3, scratch[11], scratch[12]);

        ++Fout0; ++Fout1; ++Fout2; ++Fout3; ++Fout4;
    }
}

/* Generic DFT for prime factors not in {2,3,4,5} */
static void kf_bfly_generic(kiss_fft_cpx *Fout, const size_t fstride,
                            const kiss_fft_cfg st, int m, int p)
{
    int u, k, q1, q;
    kiss_fft_cpx *twiddles = st->twiddles;
    kiss_fft_cpx t;
    int Norig = st->nfft;

    kiss_fft_cpx *scratch = (kiss_fft_cpx *)malloc(sizeof(kiss_fft_cpx) * p);

    for (u = 0; u < m; ++u) {
        k = u;
        for (q1 = 0; q1 < p; ++q1) {
            scratch[q1] = Fout[k];
            k += m;
        }

        k = u;
        for (q1 = 0; q1 < p; ++q1) {
            int twidx = 0;
            Fout[k] = scratch[0];
            for (q = 1; q < p; ++q) {
                twidx += (int)fstride * k;
                if (twidx >= Norig) twidx -= Norig;
                C_MUL(t, scratch[q], twiddles[twidx]);
                C_ADDTO(Fout[k], t);
            }
            k += m;
        }
    }
    free(scratch);
}

static void kf_work(kiss_fft_cpx *Fout, const kiss_fft_cpx *f,
                    const size_t fstride, int in_stride,
                    int *factors, const kiss_fft_cfg st)
{
    kiss_fft_cpx *Fout_beg = Fout;
    const int p = *factors++;
    const int m = *factors++;
    const kiss_fft_cpx *Fout_end = Fout + p * m;

    if (m == 1) {
        do {
            *Fout = *f;
            f += fstride * in_stride;
        } while (++Fout != Fout_end);
    } else {
        do {
            kf_work(Fout, f, fstride * p, in_stride, factors, st);
            f += fstride * in_stride;
        } while ((Fout += m) != Fout_end);
    }

    Fout = Fout_beg;

    if (p == 2) kf_bfly2(Fout, fstride, st, m);
    else if (p == 3) kf_bfly3(Fout, fstride, st, m);
    else if (p == 4) kf_bfly4(Fout, fstride, st, m);
    else if (p == 5) kf_bfly5(Fout, fstride, st, m);
    else kf_bfly_generic(Fout, fstride, st, m, p);
}

static void kf_factor(int n, int *facbuf)
{
    int p = 4;
    double floor_sqrt;
    floor_sqrt = floor(sqrt((double)n));

    while (n > 1) {
        while (n % p) {
            switch (p) {
            case 4: p = 2; break;
            case 2: p = 3; break;
            default: p += 2; break;
            }
            if (p > floor_sqrt) p = n;
        }
        n /= p;
        *facbuf++ = p;
        *facbuf++ = n;
    }
}

kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft,
                            void *mem, size_t *lenmem)
{
    kiss_fft_cfg st = NULL;
    size_t memneeded = sizeof(struct kiss_fft_state) +
                       sizeof(kiss_fft_cpx) * (nfft - 1);

    if (lenmem == NULL) {
        st = (kiss_fft_cfg)malloc(memneeded);
    } else {
        if (mem != NULL && *lenmem >= memneeded)
            st = (kiss_fft_cfg)mem;
        *lenmem = memneeded;
    }

    if (st) {
        int i;
        st->nfft = nfft;
        st->inverse = inverse_fft;

        for (i = 0; i < nfft; ++i) {
            double phase = -2.0 * 3.14159265358979323846 * i / nfft;
            if (st->inverse) phase *= -1;
            kf_cexp(st->twiddles + i, phase);
        }

        kf_factor(nfft, st->factors);
    }
    return st;
}

void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx *fin, kiss_fft_cpx *fout)
{
    kiss_fft_stride(cfg, fin, fout, 1);
}

void kiss_fft_stride(kiss_fft_cfg cfg, const kiss_fft_cpx *fin,
                     kiss_fft_cpx *fout, int fin_stride)
{
    if (fin == fout) {
        /* in-place not directly supported, use temp buffer */
        kiss_fft_cpx *tmpbuf = (kiss_fft_cpx *)malloc(sizeof(kiss_fft_cpx) * cfg->nfft);
        kf_work(tmpbuf, fin, 1, fin_stride, cfg->factors, cfg);
        memcpy(fout, tmpbuf, sizeof(kiss_fft_cpx) * cfg->nfft);
        free(tmpbuf);
    } else {
        kf_work(fout, fin, 1, fin_stride, cfg->factors, cfg);
    }
}

void kiss_fft_free(void *p)
{
    free(p);
}

int kiss_fft_next_fast_size(int n)
{
    while (1) {
        int m = n;
        while ((m % 2) == 0) m /= 2;
        while ((m % 3) == 0) m /= 3;
        while ((m % 5) == 0) m /= 5;
        if (m <= 1) break;
        n++;
    }
    return n;
}
