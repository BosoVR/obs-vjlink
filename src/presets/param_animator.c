
#include "param_animator.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static struct vjlink_lfo g_lfos[VJLINK_NUM_LFOS];
static struct vjlink_beat_envelope g_beat_env;
static struct vjlink_sequencer g_sequencers[VJLINK_NUM_LFOS]; /* one per LFO slot */
static bool g_initialized = false;

static void init_defaults(void)
{
	if (g_initialized)
		return;

	for (int i = 0; i < VJLINK_NUM_LFOS; i++) {
		g_lfos[i].waveform = VJLINK_LFO_SINE;
		g_lfos[i].frequency = 0.5f;
		g_lfos[i].phase_offset = 0.0f;
		g_lfos[i].sync_to_beat = false;
		g_lfos[i].current_value = 0.0f;
		g_lfos[i].random_value = 0.0f;
		g_lfos[i].random_timer = 0.0f;
	}

	g_beat_env.decay_rate = 0.1f;
	g_beat_env.current_value = 0.0f;
	g_beat_env.prev_beat_phase = 0.0f;

	memset(g_sequencers, 0, sizeof(g_sequencers));

	g_initialized = true;
}

static float lfo_evaluate(struct vjlink_lfo *lfo, float phase)
{
	/* phase is 0.0 to 1.0 */
	float p = fmodf(phase + lfo->phase_offset, 1.0f);
	if (p < 0.0f) p += 1.0f;

	switch (lfo->waveform) {
	case VJLINK_LFO_SINE:
		return sinf(p * 2.0f * (float)M_PI);

	case VJLINK_LFO_TRIANGLE:
		if (p < 0.25f) return p * 4.0f;
		if (p < 0.75f) return 2.0f - p * 4.0f;
		return p * 4.0f - 4.0f;

	case VJLINK_LFO_SAWTOOTH:
		return 2.0f * p - 1.0f;

	case VJLINK_LFO_SQUARE:
		return p < 0.5f ? 1.0f : -1.0f;

	case VJLINK_LFO_RANDOM:
		return lfo->random_value;

	default:
		return 0.0f;
	}
}

void vjlink_animator_update(float dt)
{
	init_defaults();

	struct vjlink_context *ctx = vjlink_get_context();
	float elapsed = ctx->elapsed_time;
	float beat_phase = ctx->beat_phase;
	float bpm = ctx->bpm;

	/* Update LFOs */
	for (int i = 0; i < VJLINK_NUM_LFOS; i++) {
		struct vjlink_lfo *lfo = &g_lfos[i];
		float phase;

		if (lfo->sync_to_beat && bpm > 0.0f) {
			/* Sync to beat: frequency is relative to beat rate */
			float beats_per_sec = bpm / 60.0f;
			phase = fmodf(elapsed * beats_per_sec * lfo->frequency, 1.0f);
		} else {
			phase = fmodf(elapsed * lfo->frequency, 1.0f);
		}

		/* Update random waveform periodically */
		if (lfo->waveform == VJLINK_LFO_RANDOM) {
			lfo->random_timer += dt;
			float period = 1.0f / (lfo->frequency + 0.001f);
			if (lfo->random_timer >= period) {
				lfo->random_timer -= period;
				lfo->random_value = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
			}
		}

		lfo->current_value = lfo_evaluate(lfo, phase);

		/* Write to global context for shader access */
		ctx->lfo_values[i] = (lfo->current_value + 1.0f) * 0.5f; /* remap to 0..1 */
	}

	/* Update beat envelope */
	if (beat_phase < g_beat_env.prev_beat_phase - 0.5f) {
		/* Beat phase wrapped around (new beat detected) */
		g_beat_env.current_value = 1.0f;
	}
	g_beat_env.prev_beat_phase = beat_phase;

	/* Exponential decay */
	g_beat_env.current_value *= (1.0f - g_beat_env.decay_rate);
	if (g_beat_env.current_value < 0.001f)
		g_beat_env.current_value = 0.0f;

	/* Update sequencers: advance step on beat */
	for (int i = 0; i < VJLINK_NUM_LFOS; i++) {
		struct vjlink_sequencer *seq = &g_sequencers[i];
		if (!seq->active || seq->step_count <= 0)
			continue;

		if (beat_phase < seq->prev_beat_phase - 0.5f) {
			seq->current_step = (seq->current_step + 1) % seq->step_count;
		}
		seq->prev_beat_phase = beat_phase;
	}
}

float vjlink_lfo_get_value(int lfo_index)
{
	if (lfo_index < 0 || lfo_index >= VJLINK_NUM_LFOS)
		return 0.0f;
	return (g_lfos[lfo_index].current_value + 1.0f) * 0.5f; /* 0..1 */
}

void vjlink_lfo_set(int index, enum vjlink_lfo_waveform waveform,
                    float frequency, float phase_offset, bool sync_to_beat)
{
	init_defaults();
	if (index < 0 || index >= VJLINK_NUM_LFOS)
		return;

	g_lfos[index].waveform = waveform;
	g_lfos[index].frequency = frequency;
	g_lfos[index].phase_offset = phase_offset;
	g_lfos[index].sync_to_beat = sync_to_beat;
}

float vjlink_beat_envelope_get(float decay_rate)
{
	/* Use custom decay rate if provided, otherwise use default */
	if (decay_rate > 0.0f && decay_rate != g_beat_env.decay_rate) {
		/* Compute independent envelope with this decay rate */
		/* Simplified: just return current value scaled */
		return g_beat_env.current_value;
	}
	return g_beat_env.current_value;
}

float vjlink_animate_param(const char *source_type, int band_index,
                           int lfo_index, float scale, float offset,
                           float decay_rate)
{
	struct vjlink_context *ctx = vjlink_get_context();

	if (strcmp(source_type, "lfo") == 0) {
		float val = vjlink_lfo_get_value(lfo_index);
		return val * scale + offset;
	}

	if (strcmp(source_type, "audio_band") == 0) {
		if (band_index >= 0 && band_index < VJLINK_NUM_BANDS) {
			float val = ctx->bands[band_index];
			return val * scale + offset;
		}
		return offset;
	}

	if (strcmp(source_type, "beat_env") == 0) {
		float val = vjlink_beat_envelope_get(decay_rate);
		return val * scale + offset;
	}

	if (strcmp(source_type, "rms") == 0) {
		float val = ctx->rms;
		return val * scale + offset;
	}

	if (strcmp(source_type, "sequencer") == 0) {
		float val = vjlink_sequencer_get_value(lfo_index);
		return val * scale + offset;
	}

	return offset;
}

void vjlink_sequencer_set(int seq_index, const float *steps,
                          int step_count)
{
	init_defaults();
	if (seq_index < 0 || seq_index >= VJLINK_NUM_LFOS)
		return;
	if (!steps || step_count <= 0)
		return;

	struct vjlink_sequencer *seq = &g_sequencers[seq_index];
	seq->step_count = step_count > VJLINK_SEQ_MAX_STEPS
		? VJLINK_SEQ_MAX_STEPS : step_count;

	for (int i = 0; i < seq->step_count; i++)
		seq->steps[i] = steps[i];

	seq->current_step = 0;
	seq->prev_beat_phase = 0.0f;
	seq->active = true;
}

float vjlink_sequencer_get_value(int seq_index)
{
	if (seq_index < 0 || seq_index >= VJLINK_NUM_LFOS)
		return 0.0f;

	struct vjlink_sequencer *seq = &g_sequencers[seq_index];
	if (!seq->active || seq->step_count <= 0)
		return 0.0f;

	return seq->steps[seq->current_step];
}
