
#include "audio_engine.h"
#include "bpm_detector.h"
#include "kiss_fft.h"
#include "../vjlink_context.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <obs-module.h>

/* Band bin ranges for 48kHz sample rate, 2048-point FFT (23.4Hz per bin) */
#define BAND_BASS_START     1
#define BAND_BASS_END       10   /* 23-234Hz */
#define BAND_LOWMID_START   11
#define BAND_LOWMID_END     42   /* 234-984Hz */
#define BAND_HIGHMID_START  43
#define BAND_HIGHMID_END    170  /* 984-3984Hz */
#define BAND_TREBLE_START   171
#define BAND_TREBLE_END     512  /* 3984-12kHz */

/* Per-band gain compensation.
 * Bass has 10 bins, Treble has 342 bins - RMS averaging suppresses
 * bands with many bins. Additionally, music naturally has less
 * high-frequency energy (pink noise roll-off ~3dB/octave).
 * These gains equalize the bands for typical music. */
#define GAIN_BASS     1.0f
#define GAIN_LOWMID   1.5f
#define GAIN_HIGHMID  2.8f
#define GAIN_TREBLE   5.0f

/* Smoothing rates.
 * Attack < 1.0 prevents instant jumps that make effects frantic.
 * Decay > 0.9 gives smooth falloff that follows musical dynamics. */
#define ATTACK_RATE  0.4f
#define DECAY_RATE   0.93f

/* Peak hold decay per frame */
#define PEAK_DECAY   0.995f

/* Chronotensity parameters (AudioLink-inspired cumulative energy) */
#define CHRONO_RISE_RATE   0.15f   /* how fast chronotensity rises on energy */
#define CHRONO_DECAY_RATE  0.992f  /* slow decay in silence */
#define CHRONO_MAX         10.0f   /* max accumulation before wrapping */

struct vjlink_audio_engine {
	/* FFT state */
	kiss_fft_cfg     fft_cfg;
	kiss_fft_cpx     fft_input[VJLINK_FFT_SIZE];
	kiss_fft_cpx     fft_output[VJLINK_FFT_SIZE];
	float            fft_magnitudes[VJLINK_FFT_SIZE / 2 + 1];

	/* Hann window (precomputed) */
	float            window[VJLINK_FFT_SIZE];

	/* Ring buffer for incoming PCM samples */
	float            pcm_ring[VJLINK_FFT_SIZE];
	uint32_t         pcm_write_pos;
	uint32_t         pcm_samples_since_fft;

	/* Smoothed band values */
	float            bands_smoothed[VJLINK_NUM_BANDS];
	float            bands_raw[VJLINK_NUM_BANDS];
	float            bands_peak[VJLINK_NUM_BANDS];
	float            chronotensity[VJLINK_NUM_BANDS];
	float            onset_strength;
	float            rms;

	/* BPM detector */
	struct vjlink_bpm_detector *bpm;

	/* Sample rate */
	uint32_t         sample_rate;

	/* History buffer (for audio texture row 3) */
	float            band_history[VJLINK_AUDIO_TEX_WIDTH][VJLINK_NUM_BANDS];
	uint32_t         history_write_pos;
};

static void compute_hann_window(float *window, int size)
{
	for (int i = 0; i < size; i++) {
		window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (size - 1)));
	}
}

static float compute_band_energy(const float *magnitudes, int start, int end)
{
	float sum = 0.0f;
	int count = end - start + 1;
	for (int i = start; i <= end; i++) {
		sum += magnitudes[i] * magnitudes[i]; /* power */
	}
	return sqrtf(sum / count); /* RMS of magnitudes */
}

static float db_to_normalized(float rms_val)
{
	if (rms_val < 1e-10f)
		return 0.0f;
	float db = 20.0f * log10f(rms_val);
	/* Map -60dB..0dB to 0.0..1.0 */
	float normalized = (db + 60.0f) / 60.0f;
	if (normalized < 0.0f) normalized = 0.0f;
	if (normalized > 1.0f) normalized = 1.0f;
	return normalized;
}

struct vjlink_audio_engine *vjlink_audio_engine_create(uint32_t sample_rate)
{
	struct vjlink_audio_engine *engine = calloc(1, sizeof(*engine));
	if (!engine)
		return NULL;

	engine->sample_rate = sample_rate;

	/* Initialize FFT */
	engine->fft_cfg = kiss_fft_alloc(VJLINK_FFT_SIZE, 0, NULL, NULL);
	if (!engine->fft_cfg) {
		free(engine);
		return NULL;
	}

	/* Precompute Hann window */
	compute_hann_window(engine->window, VJLINK_FFT_SIZE);

	/* Create BPM detector */
	engine->bpm = vjlink_bpm_detector_create(sample_rate);

	blog(LOG_INFO, "[VJLink] Audio engine created (sample rate: %u)", sample_rate);
	return engine;
}

void vjlink_audio_engine_destroy(struct vjlink_audio_engine *engine)
{
	if (!engine)
		return;

	if (engine->bpm)
		vjlink_bpm_detector_destroy(engine->bpm);

	if (engine->fft_cfg)
		kiss_fft_free(engine->fft_cfg);

	free(engine);
}

static void run_fft(struct vjlink_audio_engine *engine)
{
	/* Apply Hann window and copy to FFT input */
	uint32_t read_pos = engine->pcm_write_pos;
	for (int i = 0; i < VJLINK_FFT_SIZE; i++) {
		uint32_t idx = (read_pos + i) % VJLINK_FFT_SIZE;
		engine->fft_input[i].r = engine->pcm_ring[idx] * engine->window[i];
		engine->fft_input[i].i = 0.0f;
	}

	/* Run FFT */
	kiss_fft(engine->fft_cfg, engine->fft_input, engine->fft_output);

	/* Compute magnitudes */
	int half = VJLINK_FFT_SIZE / 2;
	for (int i = 0; i <= half; i++) {
		float re = engine->fft_output[i].r;
		float im = engine->fft_output[i].i;
		engine->fft_magnitudes[i] = sqrtf(re * re + im * im) / half;
	}
}

static float compute_band_peak(const float *magnitudes, int start, int end)
{
	float peak = 0.0f;
	for (int i = start; i <= end; i++) {
		if (magnitudes[i] > peak)
			peak = magnitudes[i];
	}
	return peak;
}

static void extract_bands(struct vjlink_audio_engine *engine)
{
	float raw[VJLINK_NUM_BANDS];
	static const float band_gains[VJLINK_NUM_BANDS] = {
		GAIN_BASS, GAIN_LOWMID, GAIN_HIGHMID, GAIN_TREBLE
	};

	/* Use combination of RMS energy + peak for better reactivity.
	 * RMS gives overall energy, peak catches transients.
	 * Mix: 60% RMS + 40% peak for more responsive display. */
	float rms_vals[VJLINK_NUM_BANDS];
	float peak_vals[VJLINK_NUM_BANDS];

	rms_vals[VJLINK_BAND_BASS] = compute_band_energy(engine->fft_magnitudes,
		BAND_BASS_START, BAND_BASS_END);
	rms_vals[VJLINK_BAND_LOWMID] = compute_band_energy(engine->fft_magnitudes,
		BAND_LOWMID_START, BAND_LOWMID_END);
	rms_vals[VJLINK_BAND_HIGHMID] = compute_band_energy(engine->fft_magnitudes,
		BAND_HIGHMID_START, BAND_HIGHMID_END);
	rms_vals[VJLINK_BAND_TREBLE] = compute_band_energy(engine->fft_magnitudes,
		BAND_TREBLE_START, BAND_TREBLE_END);

	peak_vals[VJLINK_BAND_BASS] = compute_band_peak(engine->fft_magnitudes,
		BAND_BASS_START, BAND_BASS_END);
	peak_vals[VJLINK_BAND_LOWMID] = compute_band_peak(engine->fft_magnitudes,
		BAND_LOWMID_START, BAND_LOWMID_END);
	peak_vals[VJLINK_BAND_HIGHMID] = compute_band_peak(engine->fft_magnitudes,
		BAND_HIGHMID_START, BAND_HIGHMID_END);
	peak_vals[VJLINK_BAND_TREBLE] = compute_band_peak(engine->fft_magnitudes,
		BAND_TREBLE_START, BAND_TREBLE_END);

	/* Mix RMS and peak, apply per-band gain + user sensitivity */
	struct vjlink_context *ctx = vjlink_get_context();
	float master_gain = ctx->audio_master_gain;
	if (master_gain < 0.01f)
		master_gain = 0.01f;
	if (master_gain > 5.0f)
		master_gain = 5.0f;
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		float mixed = rms_vals[i] * 0.6f + peak_vals[i] * 0.4f;
		mixed *= band_gains[i] * master_gain;
		float sens = ctx->band_sensitivity[i];
		if (sens > 0.01f)
			mixed *= sens;
		raw[i] = db_to_normalized(mixed);
	}

	/* Store truly raw values (no smoothing) */
	for (int i = 0; i < VJLINK_NUM_BANDS; i++)
		engine->bands_raw[i] = raw[i];

	/* Smooth with attack/decay */
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		float fall = ctx->audio_fall_rate;
		if (fall < 0.01f)
			fall = 0.01f;
		if (fall > 0.50f)
			fall = 0.50f;
		float rate = (raw[i] > engine->bands_smoothed[i])
			? ATTACK_RATE : fall;
		engine->bands_smoothed[i] += rate * (raw[i] - engine->bands_smoothed[i]);

		/* Peak hold with slow decay */
		if (engine->bands_smoothed[i] > engine->bands_peak[i])
			engine->bands_peak[i] = engine->bands_smoothed[i];
		else
			engine->bands_peak[i] *= PEAK_DECAY;
	}

	/* Chronotensity: cumulative energy that rises on beats, decays in silence
	 * Inspired by VRChat AudioLink's chronotensity feature.
	 * Great for effects that want "accumulated beat energy" rather than instant. */
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		float energy = engine->bands_smoothed[i];
		engine->chronotensity[i] *= CHRONO_DECAY_RATE;
		engine->chronotensity[i] += energy * CHRONO_RISE_RATE;
		if (engine->chronotensity[i] > CHRONO_MAX)
			engine->chronotensity[i] -= CHRONO_MAX; /* wrap */
	}

	/* Per-band onset detection: spike when raw band exceeds smoothed by margin.
	 * - kick from bass, snare from low+highmid combo, hat from treble. */
	{
		float kick_diff = raw[VJLINK_BAND_BASS] - engine->bands_smoothed[VJLINK_BAND_BASS];
		float snare_diff = ((raw[VJLINK_BAND_LOWMID] + raw[VJLINK_BAND_HIGHMID]) * 0.5f)
		                 - ((engine->bands_smoothed[VJLINK_BAND_LOWMID]
		                   + engine->bands_smoothed[VJLINK_BAND_HIGHMID]) * 0.5f);
		float hat_diff = raw[VJLINK_BAND_TREBLE] - engine->bands_smoothed[VJLINK_BAND_TREBLE];

		float kick = kick_diff > 0.05f ? kick_diff * 4.0f : 0.0f;
		float snare = snare_diff > 0.04f ? snare_diff * 4.0f : 0.0f;
		float hat = hat_diff > 0.04f ? hat_diff * 4.0f : 0.0f;

		if (kick > 1.0f) kick = 1.0f;
		if (snare > 1.0f) snare = 1.0f;
		if (hat > 1.0f) hat = 1.0f;

		/* Decay last frame's value, take max with new */
		ctx->kick_onset = (ctx->kick_onset * 0.78f > kick) ? ctx->kick_onset * 0.78f : kick;
		ctx->snare_onset = (ctx->snare_onset * 0.72f > snare) ? ctx->snare_onset * 0.72f : snare;
		ctx->hat_onset = (ctx->hat_onset * 0.65f > hat) ? ctx->hat_onset * 0.65f : hat;
	}
}

static void write_audio_texture(struct vjlink_audio_engine *engine)
{
	struct vjlink_context *ctx = vjlink_get_context();
	long write_idx = ctx->audio_write_idx;
	float *buf = ctx->audio_cpu_buffer[write_idx];
	int w = VJLINK_AUDIO_TEX_WIDTH;

	/* Clear buffer */
	memset(buf, 0, VJLINK_AUDIO_TEX_PIXELS * 4 * sizeof(float));

	int half = VJLINK_FFT_SIZE / 2;

	/* Row 0: FFT spectrum (first 512 bins, R channel = magnitude) */
	for (int i = 0; i < w && i <= half; i++) {
		int px = (0 * w + i) * 4; /* row 0, pixel i, RGBA */
		buf[px + 0] = engine->fft_magnitudes[i]; /* R = magnitude */
		buf[px + 1] = 0.0f; /* G */
		buf[px + 2] = 0.0f; /* B */
		buf[px + 3] = 1.0f; /* A */
	}

	/* Row 1: PCM waveform (512 samples from ring buffer) */
	for (int i = 0; i < w; i++) {
		int pcm_idx = (engine->pcm_write_pos + i * (VJLINK_FFT_SIZE / w))
			% VJLINK_FFT_SIZE;
		int px = (1 * w + i) * 4;
		buf[px + 0] = engine->pcm_ring[pcm_idx]; /* R = sample */
		buf[px + 1] = 0.0f;
		buf[px + 2] = 0.0f;
		buf[px + 3] = 1.0f;
	}

	/* Row 2: Aggregate data */
	/* Pixels 0-3: Bass, LowMid, HighMid, Treble (smoothed in R, peak in G) */
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		int px = (2 * w + i) * 4;
		buf[px + 0] = engine->bands_smoothed[i]; /* R = smoothed */
		buf[px + 1] = engine->bands_peak[i];     /* G = peak */
		buf[px + 2] = 0.0f;
		buf[px + 3] = 1.0f;
	}

	/* Pixels 4-7: Chronotensity per band (R = chronotensity normalized) */
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		int px = (2 * w + 4 + i) * 4;
		buf[px + 0] = engine->chronotensity[i] / CHRONO_MAX; /* R = normalized 0-1 */
		buf[px + 1] = engine->bands_raw[i];                   /* G = raw band value */
		buf[px + 2] = engine->onset_strength;                  /* B = onset strength */
		buf[px + 3] = 1.0f;
	}

	/* Pixel 8: beat_phase(R), bpm_normalized(G), beat_confidence(B), rms(A) */
	{
		int px = (2 * w + 8) * 4;
		buf[px + 0] = ctx->beat_phase;
		buf[px + 1] = ctx->bpm / 300.0f; /* normalize BPM to 0-1 (max 300) */
		buf[px + 2] = ctx->beat_confidence;
		buf[px + 3] = engine->rms;
	}

	/* Row 3: Band history (scrolling) */
	uint32_t hist_pos = engine->history_write_pos;
	for (int i = 0; i < w; i++) {
		int hist_idx = (hist_pos + i) % w;
		int px = (3 * w + i) * 4;
		buf[px + 0] = engine->band_history[hist_idx][0]; /* R = bass */
		buf[px + 1] = engine->band_history[hist_idx][1]; /* G = lowmid */
		buf[px + 2] = engine->band_history[hist_idx][2]; /* B = highmid */
		buf[px + 3] = engine->band_history[hist_idx][3]; /* A = treble */
	}

	/* Update global context bands */
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		ctx->bands[i] = engine->bands_smoothed[i];
		ctx->bands_peak[i] = engine->bands_peak[i];
		ctx->bands_raw[i] = engine->bands_raw[i];
		ctx->chronotensity[i] = engine->chronotensity[i] / CHRONO_MAX;
	}
	ctx->onset_strength = engine->onset_strength;
	ctx->rms = engine->rms;
}

void vjlink_audio_engine_process(struct vjlink_audio_engine *engine,
                                 const float *data,
                                 uint32_t frames,
                                 uint32_t channels)
{
	if (!engine || !data || frames == 0)
		return;

	/* Mix down to mono and write to ring buffer */
	for (uint32_t f = 0; f < frames; f++) {
		float sample = 0.0f;
		for (uint32_t c = 0; c < channels; c++) {
			sample += data[f * channels + c];
		}
		sample /= (float)channels;

		engine->pcm_ring[engine->pcm_write_pos] = sample;
		engine->pcm_write_pos = (engine->pcm_write_pos + 1) % VJLINK_FFT_SIZE;
		engine->pcm_samples_since_fft++;
	}

	/* Compute RMS over incoming frames */
	{
		float sum_sq = 0.0f;
		for (uint32_t f = 0; f < frames; f++) {
			float sample = 0.0f;
			for (uint32_t c = 0; c < channels; c++)
				sample += data[f * channels + c];
			sample /= (float)channels;
			sum_sq += sample * sample;
		}
		float raw_rms = sqrtf(sum_sq / frames);
		float norm_rms = db_to_normalized(raw_rms);
		engine->rms += 0.5f * (norm_rms - engine->rms);
	}

	/* Run FFT every VJLINK_FFT_HOP samples */
	if (engine->pcm_samples_since_fft >= VJLINK_FFT_HOP) {
		engine->pcm_samples_since_fft = 0;

		run_fft(engine);
		extract_bands(engine);

		/* Feed spectral flux to BPM detector */
		if (engine->bpm) {
			vjlink_bpm_detector_process(engine->bpm,
				engine->fft_magnitudes,
				VJLINK_FFT_SIZE / 2 + 1);

			struct vjlink_context *ctx = vjlink_get_context();
			float old_phase = ctx->beat_phase;
			float new_phase = vjlink_bpm_detector_get_beat_phase(engine->bpm);
			ctx->beat_phase = new_phase;
			ctx->bpm = vjlink_bpm_detector_get_bpm(engine->bpm);
			ctx->beat_confidence = vjlink_bpm_detector_get_confidence(engine->bpm);

			/* Store onset strength for effects */
			engine->onset_strength = vjlink_bpm_detector_get_onset_strength(engine->bpm);

			/* BPM-derived subdivisions. beat_phase wraps 0->1 each beat;
			 * we count beats and derive 1/8, 1/16, 2-beat, 4-beat phases. */
			if (new_phase < old_phase) {
				ctx->beat_count++;
			}
			ctx->beat_1_4 = new_phase;
			float p2 = new_phase * 2.0f;
			ctx->beat_1_8 = p2 - (float)((int)p2);
			float p4 = new_phase * 4.0f;
			ctx->beat_1_16 = p4 - (float)((int)p4);
			ctx->beat_2_1 = ((float)(ctx->beat_count & 1) + new_phase) * 0.5f;
			ctx->beat_4_1 = ((float)(ctx->beat_count & 3) + new_phase) * 0.25f;
		}

		/* Update band history */
		uint32_t hp = engine->history_write_pos;
		for (int i = 0; i < VJLINK_NUM_BANDS; i++)
			engine->band_history[hp][i] = engine->bands_smoothed[i];
		engine->history_write_pos = (hp + 1) % VJLINK_AUDIO_TEX_WIDTH;

		/* Write audio texture CPU buffer */
		write_audio_texture(engine);

		/* Swap double buffer */
		vjlink_audio_buffer_swap();
	}
}

void vjlink_audio_engine_get_bands(struct vjlink_audio_engine *engine,
                                   float *bands_out)
{
	if (!engine || !bands_out)
		return;
	for (int i = 0; i < VJLINK_NUM_BANDS; i++)
		bands_out[i] = engine->bands_smoothed[i];
}

float vjlink_audio_engine_get_rms(struct vjlink_audio_engine *engine)
{
	return engine ? engine->rms : 0.0f;
}
