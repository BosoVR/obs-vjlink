#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BPM Detector
 *
 * Pipeline:
 * 1. Spectral flux onset detection (half-wave rectified)
 * 2. Adaptive threshold (median filter)
 * 3. Inter-onset interval histogram
 * 4. Autocorrelation for BPM estimation
 * 5. Phase-locked loop for beat phase tracking
 *
 * Called from the audio thread every FFT hop.
 */

struct vjlink_bpm_detector;

/* Create / destroy */
struct vjlink_bpm_detector *vjlink_bpm_detector_create(uint32_t sample_rate);
void vjlink_bpm_detector_destroy(struct vjlink_bpm_detector *det);

/*
 * Process a new FFT frame's magnitudes.
 * Called every FFT hop from the audio engine.
 *
 * magnitudes: FFT magnitude array (half-spectrum + DC)
 * num_bins: number of magnitude bins
 */
void vjlink_bpm_detector_process(struct vjlink_bpm_detector *det,
                                 const float *magnitudes,
                                 uint32_t num_bins);

/* Get detected BPM (0 if not yet detected) */
float vjlink_bpm_detector_get_bpm(struct vjlink_bpm_detector *det);

/* Get beat phase (0.0 = on beat, ramps to 1.0, resets) */
float vjlink_bpm_detector_get_beat_phase(struct vjlink_bpm_detector *det);

/* Get detection confidence (0.0 - 1.0) */
float vjlink_bpm_detector_get_confidence(struct vjlink_bpm_detector *det);

/* Get current onset strength (0.0 - 1.0, spikes on detected onsets) */
float vjlink_bpm_detector_get_onset_strength(struct vjlink_bpm_detector *det);

#ifdef __cplusplus
}
#endif
