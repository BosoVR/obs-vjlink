
#include "bpm_detector.h"
#include "vjlink_context.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <obs-module.h>

/* Onset detection parameters */
#define ONSET_HISTORY_SIZE  512  /* ~5.3 seconds at 10ms hop */
#define MEDIAN_WINDOW       7
#define ONSET_THRESHOLD_MUL 1.3f

/* Autocorrelation parameters */
#define AUTOCORR_SIZE       384  /* lags to test (~4 seconds) */
#define MIN_BPM             80.0f
#define MAX_BPM             230.0f  /* hardtechno/rawstyle range */

/* IOI histogram */
#define IOI_HIST_BINS       600  /* 5ms bins, covers 0-3000ms */
#define IOI_BIN_MS          5.0f
#define IOI_MAX_INTERVAL_MS 2000.0f

/* PLL parameters */
#define PLL_CORRECTION_RATE 0.05f

/* Silence detection */
#define SILENCE_THRESHOLD   0.005f  /* flux below this = silence */
#define SILENCE_RESET_HOPS  200     /* ~2 seconds of silence before reset */

struct vjlink_bpm_detector {
	/* Previous FFT magnitudes (for spectral flux) */
	float    *prev_magnitudes;
	uint32_t  num_bins;

	/* Onset strength history (circular buffer) */
	float     onset_history[ONSET_HISTORY_SIZE];
	uint32_t  onset_write_pos;
	uint32_t  onset_count;

	/* Inter-onset interval histogram */
	float     ioi_histogram[IOI_HIST_BINS];

	/* Onset timestamps (in hop units) for IOI calculation */
	uint32_t  onset_times[ONSET_HISTORY_SIZE];
	uint32_t  onset_time_count;
	uint32_t  onset_time_write_pos;

	/* Autocorrelation result */
	float     autocorr[AUTOCORR_SIZE];

	/* Current hop count (monotonically increasing) */
	uint64_t  hop_count;

	/* Detected BPM and confidence */
	float     bpm;
	float     confidence;

	/* Phase-locked loop state */
	float     beat_phase;       /* 0.0 - 1.0 */
	float     beat_period_hops; /* expected beat interval in hops */
	float     phase_increment;  /* per-hop phase step */

	/* Onset strength (for external use) */
	float     onset_strength;     /* 0-1, decays after onset */

	/* Silence detection */
	uint32_t  silence_counter;    /* hops since last onset */

	/* Timing */
	float     hop_duration_ms;  /* duration of one hop in milliseconds */
	uint32_t  sample_rate;
};

static int float_compare(const void *a, const void *b)
{
	float fa = *(const float *)a;
	float fb = *(const float *)b;
	if (fa < fb) return -1;
	if (fa > fb) return 1;
	return 0;
}

static float median_filter(const float *data, uint32_t pos, uint32_t size, int window)
{
	float sorted[MEDIAN_WINDOW];
	int half = window / 2;
	int count = 0;

	for (int i = -half; i <= half; i++) {
		int idx = (int)pos + i;
		if (idx < 0) idx += (int)size;
		else if ((uint32_t)idx >= size) idx -= (int)size;
		sorted[count++] = data[idx];
	}

	qsort(sorted, count, sizeof(float), float_compare);
	return sorted[count / 2];
}

struct vjlink_bpm_detector *vjlink_bpm_detector_create(uint32_t sample_rate)
{
	struct vjlink_bpm_detector *det = calloc(1, sizeof(*det));
	if (!det)
		return NULL;

	det->sample_rate = sample_rate;
	det->hop_duration_ms = (float)VJLINK_FFT_HOP / (float)sample_rate * 1000.0f;
	det->bpm = 0.0f;
	det->confidence = 0.0f;
	det->beat_phase = 0.0f;
	det->beat_period_hops = 0.0f;
	det->phase_increment = 0.0f;

	blog(LOG_INFO, "[VJLink] BPM detector created (hop: %.1fms)", det->hop_duration_ms);
	return det;
}

void vjlink_bpm_detector_destroy(struct vjlink_bpm_detector *det)
{
	if (!det)
		return;
	free(det->prev_magnitudes);
	free(det);
}

static float compute_spectral_flux(struct vjlink_bpm_detector *det,
                                   const float *magnitudes, uint32_t num_bins)
{
	float flux = 0.0f;

	if (det->prev_magnitudes && det->num_bins == num_bins) {
		for (uint32_t i = 0; i < num_bins; i++) {
			float diff = magnitudes[i] - det->prev_magnitudes[i];
			if (diff > 0.0f) {
				/* Weight bass frequencies higher for kick detection */
				float weight = (i < num_bins / 8) ? 3.0f :
				               (i < num_bins / 4) ? 1.5f : 1.0f;
				flux += diff * weight;
			}
		}
	}

	/* Store current magnitudes for next frame */
	if (!det->prev_magnitudes || det->num_bins != num_bins) {
		free(det->prev_magnitudes);
		det->prev_magnitudes = malloc(num_bins * sizeof(float));
		det->num_bins = num_bins;
	}
	memcpy(det->prev_magnitudes, magnitudes, num_bins * sizeof(float));

	return flux;
}

static bool detect_onset(struct vjlink_bpm_detector *det, float flux)
{
	/* Store in history */
	det->onset_history[det->onset_write_pos] = flux;

	/* Compute adaptive threshold via median filter */
	bool is_onset = false;
	if (det->onset_count >= MEDIAN_WINDOW) {
		float threshold = median_filter(det->onset_history,
			det->onset_write_pos, ONSET_HISTORY_SIZE,
			MEDIAN_WINDOW);
		threshold *= ONSET_THRESHOLD_MUL;

		if (flux > threshold && flux > 0.01f)
			is_onset = true;
	}

	det->onset_write_pos = (det->onset_write_pos + 1) % ONSET_HISTORY_SIZE;
	if (det->onset_count < ONSET_HISTORY_SIZE)
		det->onset_count++;

	return is_onset;
}

static void update_ioi_histogram(struct vjlink_bpm_detector *det)
{
	/* Decay existing histogram */
	for (int i = 0; i < IOI_HIST_BINS; i++)
		det->ioi_histogram[i] *= 0.99f;

	/* Compute intervals between recent onsets */
	uint32_t count = det->onset_time_count;
	if (count < 2)
		return;

	/* Check intervals from the newest onset to previous onsets */
	uint32_t newest_pos = (det->onset_time_write_pos + ONSET_HISTORY_SIZE - 1) % ONSET_HISTORY_SIZE;
	uint32_t newest_time = det->onset_times[newest_pos];

	for (uint32_t i = 1; i < count && i < 32; i++) {
		uint32_t prev_pos = (newest_pos + ONSET_HISTORY_SIZE - i) % ONSET_HISTORY_SIZE;
		uint32_t prev_time = det->onset_times[prev_pos];

		float interval_ms = (float)(newest_time - prev_time) * det->hop_duration_ms;
		if (interval_ms > IOI_MAX_INTERVAL_MS)
			break;

		int bin = (int)(interval_ms / IOI_BIN_MS);
		if (bin >= 0 && bin < IOI_HIST_BINS) {
			/* Weight recent intervals higher */
			float weight = 1.0f / (float)(i);
			det->ioi_histogram[bin] += weight;
		}
	}
}

static void run_autocorrelation(struct vjlink_bpm_detector *det)
{
	/* Autocorrelation of onset strength signal */
	uint32_t n = det->onset_count;
	if (n < AUTOCORR_SIZE)
		return;

	/* Compute mean */
	float mean = 0.0f;
	for (uint32_t i = 0; i < n; i++)
		mean += det->onset_history[i];
	mean /= (float)n;

	/* Compute autocorrelation for each lag */
	float max_corr = 0.0f;
	int best_lag = 0;

	/* Lag range corresponding to MIN_BPM..MAX_BPM */
	float min_lag = 60000.0f / MAX_BPM / det->hop_duration_ms;
	float max_lag = 60000.0f / MIN_BPM / det->hop_duration_ms;
	int min_lag_i = (int)min_lag;
	int max_lag_i = (int)max_lag;
	if (max_lag_i >= AUTOCORR_SIZE) max_lag_i = AUTOCORR_SIZE - 1;

	for (int lag = min_lag_i; lag <= max_lag_i; lag++) {
		float corr = 0.0f;
		float norm_a = 0.0f;
		float norm_b = 0.0f;
		int count = 0;

		for (uint32_t i = 0; i < n - lag; i++) {
			uint32_t idx_a = (det->onset_write_pos + ONSET_HISTORY_SIZE - n + i) % ONSET_HISTORY_SIZE;
			uint32_t idx_b = (idx_a + lag) % ONSET_HISTORY_SIZE;
			float a = det->onset_history[idx_a] - mean;
			float b = det->onset_history[idx_b] - mean;
			corr += a * b;
			norm_a += a * a;
			norm_b += b * b;
			count++;
		}

		if (norm_a > 0.0f && norm_b > 0.0f) {
			corr /= sqrtf(norm_a * norm_b);
		} else {
			corr = 0.0f;
		}

		det->autocorr[lag] = corr;
		if (corr > max_corr) {
			max_corr = corr;
			best_lag = lag;
		}
	}

	if (best_lag > 0 && max_corr > 0.1f) {
		float bpm_from_autocorr = 60000.0f / (best_lag * det->hop_duration_ms);

		/* Tempo octave disambiguation:
		 * Check if half-tempo or double-tempo has similar autocorrelation.
		 * Prefer the BPM that falls in the "sweet spot" range (120-185 for
		 * hardtechno/rawstyle/hardstyle). */
		{
			int half_lag = best_lag * 2;
			int double_lag = best_lag / 2;
			float half_corr = 0.0f, double_corr = 0.0f;

			if (half_lag < AUTOCORR_SIZE)
				half_corr = det->autocorr[half_lag];
			if (double_lag >= (int)min_lag)
				double_corr = det->autocorr[double_lag];

			float half_bpm = bpm_from_autocorr / 2.0f;
			float double_bpm = bpm_from_autocorr * 2.0f;

			/* Prefer BPM in 120-185 range (hardtechno sweet spot) */
			float prefer_min = 120.0f, prefer_max = 185.0f;
			bool curr_in_range = bpm_from_autocorr >= prefer_min &&
			                     bpm_from_autocorr <= prefer_max;
			bool half_in_range = half_bpm >= prefer_min &&
			                     half_bpm <= prefer_max;
			bool double_in_range = double_bpm >= prefer_min &&
			                       double_bpm <= prefer_max;

			/* If detected BPM is outside sweet spot but octave variant
			 * is inside and has decent correlation, prefer it */
			if (!curr_in_range && half_in_range &&
			    half_corr > max_corr * 0.6f) {
				bpm_from_autocorr = half_bpm;
				max_corr = half_corr;
			} else if (!curr_in_range && double_in_range &&
			           double_corr > max_corr * 0.6f) {
				bpm_from_autocorr = double_bpm;
				max_corr = double_corr;
			}
		}

		/* Also check IOI histogram for dominant interval */
		int best_hist_bin = 0;
		float best_hist_val = 0.0f;
		int min_bin = (int)(60000.0f / MAX_BPM / IOI_BIN_MS);
		int max_bin = (int)(60000.0f / MIN_BPM / IOI_BIN_MS);
		if (max_bin >= IOI_HIST_BINS) max_bin = IOI_HIST_BINS - 1;

		for (int i = min_bin; i <= max_bin; i++) {
			if (det->ioi_histogram[i] > best_hist_val) {
				best_hist_val = det->ioi_histogram[i];
				best_hist_bin = i;
			}
		}

		float bpm_from_hist = 0.0f;
		if (best_hist_bin > 0 && best_hist_val > 0.5f) {
			bpm_from_hist = 60000.0f / (best_hist_bin * IOI_BIN_MS);
		}

		/* Weighted average (60% autocorr, 40% histogram) */
		float new_bpm;
		if (bpm_from_hist > 0.0f) {
			new_bpm = bpm_from_autocorr * 0.6f + bpm_from_hist * 0.4f;
		} else {
			new_bpm = bpm_from_autocorr;
		}

		/* Smooth BPM changes */
		if (det->bpm > 0.0f) {
			/* Only update if within 20% of current, or if confidence is low */
			float ratio = new_bpm / det->bpm;
			if (ratio > 0.8f && ratio < 1.2f) {
				det->bpm += 0.1f * (new_bpm - det->bpm);
			} else if (max_corr > det->confidence * 1.5f) {
				/* Large jump but high confidence: accept */
				det->bpm = new_bpm;
			}
		} else {
			det->bpm = new_bpm;
		}

		det->confidence = max_corr;

		/* Update PLL beat period */
		det->beat_period_hops = 60000.0f / det->bpm / det->hop_duration_ms;
		if (det->beat_period_hops > 0.0f) {
			det->phase_increment = 1.0f / det->beat_period_hops;
		}
	}
}

static void update_pll(struct vjlink_bpm_detector *det, bool is_onset)
{
	if (det->beat_period_hops <= 0.0f)
		return;

	/* Advance phase */
	det->beat_phase += det->phase_increment;

	/* Phase correction on onset */
	if (is_onset && det->confidence > 0.2f) {
		/* Calculate phase error: onset should occur near phase 0 or 1 */
		float phase_error = det->beat_phase;
		if (phase_error > 0.5f)
			phase_error -= 1.0f; /* wrap to [-0.5, 0.5] */

		/* Correct phase */
		det->beat_phase -= PLL_CORRECTION_RATE * phase_error;

		/* Slight tempo correction */
		det->phase_increment *= (1.0f - PLL_CORRECTION_RATE * 0.1f * phase_error);
	}

	/* Wrap phase to [0, 1) */
	while (det->beat_phase >= 1.0f)
		det->beat_phase -= 1.0f;
	while (det->beat_phase < 0.0f)
		det->beat_phase += 1.0f;
}

void vjlink_bpm_detector_process(struct vjlink_bpm_detector *det,
                                 const float *magnitudes,
                                 uint32_t num_bins)
{
	if (!det || !magnitudes)
		return;

	det->hop_count++;

	/* Decay onset strength */
	det->onset_strength *= 0.85f;

	/* Step 1: Spectral flux */
	float flux = compute_spectral_flux(det, magnitudes, num_bins);

	/* Step 2: Onset detection */
	bool is_onset = detect_onset(det, flux);

	/* Update onset strength on detection */
	if (is_onset) {
		det->onset_strength = fminf(1.0f, flux * 5.0f);
		det->silence_counter = 0;
	} else {
		det->silence_counter++;
	}

	/* Silence detection: reset BPM when no onsets for a while */
	if (det->silence_counter > SILENCE_RESET_HOPS) {
		det->bpm *= 0.98f;  /* fade out BPM */
		det->confidence *= 0.95f;
		det->phase_increment *= 0.98f;
		if (det->bpm < 1.0f) {
			det->bpm = 0.0f;
			det->confidence = 0.0f;
			det->beat_phase = 0.0f;
			det->phase_increment = 0.0f;
			det->beat_period_hops = 0.0f;
		}
	}

	/* Record onset timestamp */
	if (is_onset) {
		det->onset_times[det->onset_time_write_pos] = (uint32_t)(det->hop_count & 0xFFFFFFFF);
		det->onset_time_write_pos = (det->onset_time_write_pos + 1) % ONSET_HISTORY_SIZE;
		if (det->onset_time_count < ONSET_HISTORY_SIZE)
			det->onset_time_count++;
	}

	/* Step 3: Update IOI histogram on onset */
	if (is_onset)
		update_ioi_histogram(det);

	/* Step 4: Run autocorrelation periodically (every 25 hops = ~250ms) */
	if (det->hop_count % 25 == 0 && det->onset_count > 50)
		run_autocorrelation(det);

	/* Step 5: Update PLL */
	update_pll(det, is_onset);
}

float vjlink_bpm_detector_get_bpm(struct vjlink_bpm_detector *det)
{
	return det ? det->bpm : 0.0f;
}

float vjlink_bpm_detector_get_beat_phase(struct vjlink_bpm_detector *det)
{
	return det ? det->beat_phase : 0.0f;
}

float vjlink_bpm_detector_get_confidence(struct vjlink_bpm_detector *det)
{
	return det ? det->confidence : 0.0f;
}

float vjlink_bpm_detector_get_onset_strength(struct vjlink_bpm_detector *det)
{
	return det ? det->onset_strength : 0.0f;
}
