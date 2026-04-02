#pragma once

#include "vjlink_context.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parameter Animator
 *
 * Generates time-varying values from LFOs, audio bands, beat envelopes,
 * and sequencers. These values are used to modulate effect parameters.
 */

/* LFO waveform types */
enum vjlink_lfo_waveform {
	VJLINK_LFO_SINE = 0,
	VJLINK_LFO_TRIANGLE,
	VJLINK_LFO_SAWTOOTH,
	VJLINK_LFO_SQUARE,
	VJLINK_LFO_RANDOM,
};

/* LFO state */
struct vjlink_lfo {
	enum vjlink_lfo_waveform waveform;
	float frequency;      /* Hz */
	float phase_offset;   /* 0.0 - 1.0 */
	bool  sync_to_beat;
	float current_value;  /* output: -1.0 to 1.0 */
	float random_value;   /* for random waveform */
	float random_timer;
};

/* Beat envelope state */
struct vjlink_beat_envelope {
	float decay_rate;     /* 0.01 - 1.0 (lower = longer tail) */
	float current_value;  /* output: 0.0 to 1.0 */
	float prev_beat_phase;
};

/* Sequencer: list of values clocked by beats */
#define VJLINK_SEQ_MAX_STEPS 16

struct vjlink_sequencer {
	float steps[VJLINK_SEQ_MAX_STEPS];
	int   step_count;
	int   current_step;
	float prev_beat_phase;
	bool  active;
};

/* Initialize/update all LFOs (call once per frame from render thread) */
void vjlink_animator_update(float dt);

/* Get LFO value (output: 0.0 - 1.0, remapped from -1..1) */
float vjlink_lfo_get_value(int lfo_index);

/* Set LFO parameters */
void vjlink_lfo_set(int index, enum vjlink_lfo_waveform waveform,
                    float frequency, float phase_offset, bool sync_to_beat);

/* Get beat envelope value (decaying pulse on each beat) */
float vjlink_beat_envelope_get(float decay_rate);

/* Compute animated parameter value from source type */
float vjlink_animate_param(const char *source_type, int band_index,
                           int lfo_index, float scale, float offset,
                           float decay_rate);

/* Sequencer: set step values and enable */
void vjlink_sequencer_set(int seq_index, const float *steps,
                          int step_count);
float vjlink_sequencer_get_value(int seq_index);

#ifdef __cplusplus
}
#endif
