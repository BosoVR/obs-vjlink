/*
 * KissFFT - A mixed-radix Fast Fourier Transform
 * Copyright (c) 2003-2010 Mark Borgerding
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice.
 *     * Redistributions in binary form must reproduce the above copyright notice.
 *     * Neither the author nor the names of contributors may be used to endorse
 *       products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
 */
#ifndef KISS_FFT_H
#define KISS_FFT_H

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef kiss_fft_scalar
#define kiss_fft_scalar float
#endif

typedef struct {
    kiss_fft_scalar r;
    kiss_fft_scalar i;
} kiss_fft_cpx;

typedef struct kiss_fft_state *kiss_fft_cfg;

/*
 * kiss_fft_alloc
 *   Initialize a FFT (or IFFT) algorithm's cfg/state buffer.
 *
 *   nfft: the number of points in the FFT
 *   inverse_fft: 0 for forward, 1 for inverse
 *   mem: NULL to allocate, or pointer to pre-allocated buffer
 *   lenmem: pointer to size of mem (in/out)
 *
 *   Returns: a kiss_fft_cfg, or NULL on failure
 */
kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft,
                            void *mem, size_t *lenmem);

/*
 * kiss_fft
 *   Perform an FFT on a complex input buffer, store results in output buffer.
 */
void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx *fin, kiss_fft_cpx *fout);

/*
 * kiss_fft_stride
 *   A more generic version with stride (for multi-channel data).
 */
void kiss_fft_stride(kiss_fft_cfg cfg, const kiss_fft_cpx *fin,
                     kiss_fft_cpx *fout, int fin_stride);

/* Cleanup */
void kiss_fft_free(void *p);
#define kiss_fft_cleanup() ((void)0)

/* Utility: next fast FFT size >= n */
int kiss_fft_next_fast_size(int n);

#ifdef __cplusplus
}
#endif

#endif /* KISS_FFT_H */
