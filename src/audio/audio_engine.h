#pragma once

#include "vjlink_context.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Audio Engine
 *
 * Receives PCM audio data, performs FFT using KissFFT,
 * extracts 4 frequency bands (Bass, LowMid, HighMid, Treble),
 * and writes results to the CPU-side audio buffer for GPU upload.
 *
 * Thread safety: process_audio is called from the OBS audio thread.
 * It writes to audio_cpu_buffer[write_idx] and swaps atomically.
 */

struct vjlink_audio_engine;

/* Create / destroy */
struct vjlink_audio_engine *vjlink_audio_engine_create(uint32_t sample_rate);
void vjlink_audio_engine_destroy(struct vjlink_audio_engine *engine);

/*
 * Process incoming PCM audio data.
 * Called from OBS audio thread.
 *
 * data: interleaved float samples
 * frames: number of sample frames
 * channels: number of audio channels (1=mono, 2=stereo, etc.)
 */
void vjlink_audio_engine_process(struct vjlink_audio_engine *engine,
                                 const float *data,
                                 uint32_t frames,
                                 uint32_t channels);

/* Get current band values (thread-safe read) */
void vjlink_audio_engine_get_bands(struct vjlink_audio_engine *engine,
                                   float *bands_out);

/* Get current RMS level */
float vjlink_audio_engine_get_rms(struct vjlink_audio_engine *engine);

#ifdef __cplusplus
}
#endif
