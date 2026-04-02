#include "audio_filter.h"
#include "vjlink_context.h"
#include "audio/audio_engine.h"
#include <obs-module.h>

/*
 * VJLink Audio Analyzer Filter
 *
 * Attaches to any audio source (Desktop Audio, Mic, Media Source).
 * Captures PCM audio via obs_source_add_audio_capture_callback,
 * feeds it to the audio engine for FFT and BPM detection.
 *
 * The filter passes audio through unchanged (no modification).
 */

struct vjlink_audio_filter_data {
	obs_source_t                *source;
	struct vjlink_audio_engine  *engine;
	bool                         bpm_enabled;
};

static const char *vjlink_audio_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("VJLinkAudioFilter");
}

static void audio_capture_callback(void *param, obs_source_t *source,
                                   const struct audio_data *audio_data,
                                   bool muted)
{
	UNUSED_PARAMETER(source);
	struct vjlink_audio_filter_data *filter = param;

	if (!filter->engine || muted)
		return;

	/*
	 * OBS audio data: float planar format.
	 * audio_data->data[0] = channel 0 (left)
	 * audio_data->data[1] = channel 1 (right)
	 * audio_data->frames = number of sample frames
	 *
	 * We need to mix to mono. OBS uses planar float format,
	 * so we interleave and average channels.
	 */
	uint32_t frames = audio_data->frames;
	uint32_t channels = 0;

	/* Count active channels */
	for (int c = 0; c < MAX_AV_PLANES; c++) {
		if (audio_data->data[c])
			channels++;
		else
			break;
	}

	if (channels == 0 || frames == 0)
		return;

	/* Mix to mono in a temporary buffer */
	float *mono_buf = malloc(frames * sizeof(float));
	if (!mono_buf)
		return;

	for (uint32_t f = 0; f < frames; f++) {
		float sum = 0.0f;
		for (uint32_t c = 0; c < channels; c++) {
			const float *ch_data = (const float *)audio_data->data[c];
			sum += ch_data[f];
		}
		mono_buf[f] = sum / (float)channels;
	}

	/* Feed mono audio to engine (channels=1 since we already mixed) */
	vjlink_audio_engine_process(filter->engine, mono_buf, frames, 1);

	free(mono_buf);
}

static void *vjlink_audio_filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct vjlink_audio_filter_data *filter = calloc(1, sizeof(*filter));
	if (!filter)
		return NULL;

	filter->source = source;
	filter->bpm_enabled = true;

	/* Get sample rate from OBS audio info */
	struct obs_audio_info oai;
	uint32_t sample_rate = VJLINK_SAMPLE_RATE;
	if (obs_get_audio_info(&oai))
		sample_rate = oai.samples_per_sec;

	/* Create audio engine */
	filter->engine = vjlink_audio_engine_create(sample_rate);
	if (!filter->engine) {
		blog(LOG_ERROR, "[VJLink] Failed to create audio engine");
		free(filter);
		return NULL;
	}

	blog(LOG_INFO, "[VJLink] Audio filter created");
	return filter;
}

static void vjlink_audio_filter_destroy(void *data)
{
	struct vjlink_audio_filter_data *filter = data;
	if (!filter)
		return;

	/* Remove audio capture callback BEFORE destroying engine */
	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (parent) {
		obs_source_remove_audio_capture_callback(parent,
			audio_capture_callback, filter);
	}

	/* Null out engine pointer first to prevent filter_audio from using it */
	struct vjlink_audio_engine *engine = filter->engine;
	filter->engine = NULL;

	if (engine)
		vjlink_audio_engine_destroy(engine);

	/* Reset global audio state */
	struct vjlink_context *ctx = vjlink_get_context();
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		ctx->bands[i] = 0.0f;
		ctx->bands_peak[i] = 0.0f;
		ctx->bands_raw[i] = 0.0f;
		ctx->chronotensity[i] = 0.0f;
	}
	ctx->onset_strength = 0.0f;
	ctx->rms = 0.0f;
	ctx->bpm = 0.0f;
	ctx->beat_phase = 0.0f;
	ctx->beat_confidence = 0.0f;

	free(filter);
	blog(LOG_INFO, "[VJLink] Audio filter destroyed");
}

static void vjlink_audio_filter_update(void *data, obs_data_t *settings)
{
	struct vjlink_audio_filter_data *filter = data;
	filter->bpm_enabled = obs_data_get_bool(settings, "bpm_enabled");
}

static obs_properties_t *vjlink_audio_filter_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_float_slider(props, "smoothing",
		obs_module_text("VJLinkAudioFilter.Smoothing"),
		0.0, 1.0, 0.01);

	obs_properties_add_bool(props, "bpm_enabled",
		obs_module_text("VJLinkAudioFilter.BPMEnabled"));

	/* Per-band gain controls */
	obs_properties_t *gain_group = obs_properties_create();
	obs_properties_add_float_slider(gain_group, "bass_gain",
		obs_module_text("VJLinkAudioFilter.BassGain"),
		0.0, 4.0, 0.1);
	obs_properties_add_float_slider(gain_group, "lowmid_gain",
		obs_module_text("VJLinkAudioFilter.LowMidGain"),
		0.0, 4.0, 0.1);
	obs_properties_add_float_slider(gain_group, "highmid_gain",
		obs_module_text("VJLinkAudioFilter.HighMidGain"),
		0.0, 4.0, 0.1);
	obs_properties_add_float_slider(gain_group, "treble_gain",
		obs_module_text("VJLinkAudioFilter.TrebleGain"),
		0.0, 4.0, 0.1);
	obs_properties_add_group(props, "band_gains",
		"Band Gains", OBS_GROUP_NORMAL, gain_group);

	return props;
}

static void vjlink_audio_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "bpm_enabled", true);
	obs_data_set_default_double(settings, "smoothing", 0.5);
	obs_data_set_default_double(settings, "bass_gain", 1.0);
	obs_data_set_default_double(settings, "lowmid_gain", 1.0);
	obs_data_set_default_double(settings, "highmid_gain", 1.0);
	obs_data_set_default_double(settings, "treble_gain", 1.0);
}

/*
 * The filter callback is required for audio filters.
 * We add the audio capture callback when activated and remove on deactivate.
 * The filter_audio callback passes audio through unchanged.
 */
static struct obs_audio_data *vjlink_audio_filter_audio(void *data,
                                                        struct obs_audio_data *audio)
{
	struct vjlink_audio_filter_data *filter = data;

	if (!filter->engine || !audio || audio->frames == 0)
		return audio;

	uint32_t frames = audio->frames;
	uint32_t channels = 0;

	/* Count active channels (planar format) */
	for (int c = 0; c < MAX_AV_PLANES; c++) {
		if (audio->data[c])
			channels++;
		else
			break;
	}

	if (channels == 0)
		return audio;

	/* Mix to mono */
	float *mono_buf = malloc(frames * sizeof(float));
	if (!mono_buf)
		return audio;

	for (uint32_t f = 0; f < frames; f++) {
		float sum = 0.0f;
		for (uint32_t c = 0; c < channels; c++) {
			const float *ch_data = (const float *)audio->data[c];
			sum += ch_data[f];
		}
		mono_buf[f] = sum / (float)channels;
	}

	/* Feed mono audio to engine */
	vjlink_audio_engine_process(filter->engine, mono_buf, frames, 1);

	free(mono_buf);
	return audio;
}

static void vjlink_audio_filter_activate(void *data)
{
	struct vjlink_audio_filter_data *filter = data;

	/* Register audio capture on the parent source */
	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (parent) {
		obs_source_add_audio_capture_callback(parent,
			audio_capture_callback, filter);
		blog(LOG_INFO, "[VJLink] Audio capture registered on parent source");
	}
}

static void vjlink_audio_filter_deactivate(void *data)
{
	struct vjlink_audio_filter_data *filter = data;

	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (parent) {
		obs_source_remove_audio_capture_callback(parent,
			audio_capture_callback, filter);
		blog(LOG_INFO, "[VJLink] Audio capture removed from parent source");
	}
}

struct obs_source_info vjlink_audio_filter_info = {
	.id             = "vjlink_audio_filter",
	.type           = OBS_SOURCE_TYPE_FILTER,
	.output_flags   = OBS_SOURCE_AUDIO,
	.get_name       = vjlink_audio_filter_name,
	.create         = vjlink_audio_filter_create,
	.destroy        = vjlink_audio_filter_destroy,
	.update         = vjlink_audio_filter_update,
	.get_properties = vjlink_audio_filter_properties,
	.get_defaults   = vjlink_audio_filter_defaults,
	.filter_audio   = vjlink_audio_filter_audio,
	.activate       = vjlink_audio_filter_activate,
	.deactivate     = vjlink_audio_filter_deactivate,
};
