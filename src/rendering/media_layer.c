#include "media_layer.h"
#include <obs-module.h>
#include <string.h>
#include <math.h>

void vjlink_media_layers_init(struct vjlink_media_layers *ml,
                               uint32_t width, uint32_t height)
{
	if (!ml)
		return;

	memset(ml, 0, sizeof(*ml));
	ml->width = width;
	ml->height = height;

	for (int i = 0; i < VJLINK_MAX_MEDIA_LAYERS; i++) {
		ml->layers[i].opacity = 1.0f;
		ml->layers[i].scale = 1.0f;
		ml->layers[i].pos_x = 0.5f;
		ml->layers[i].pos_y = 0.5f;
		ml->layers[i].blend_mode = VJLINK_BLEND_NORMAL;
		ml->layers[i].threshold = 0.3f;
		ml->layers[i].intensity = 1.0f;
	}

	ml->initialized = true;
	blog(LOG_INFO, "[VJLink] Media layers initialized");
}

void vjlink_media_layers_destroy(struct vjlink_media_layers *ml)
{
	if (!ml)
		return;

	for (int i = 0; i < VJLINK_MAX_MEDIA_LAYERS; i++) {
		if (ml->layers[i].loaded) {
			obs_enter_graphics();
			gs_image_file_free(&ml->layers[i].image);
			obs_leave_graphics();
			ml->layers[i].loaded = false;
		}
	}

	ml->initialized = false;
}

void vjlink_media_layer_set(struct vjlink_media_layers *ml, int index,
                             const char *path,
                             enum vjlink_media_trigger_mode trigger_mode,
                             int trigger_band, float threshold,
                             float intensity)
{
	if (!ml || index < 0 || index >= VJLINK_MAX_MEDIA_LAYERS)
		return;

	struct vjlink_media_layer *layer = &ml->layers[index];

	/* Clear previous if path changed */
	if (layer->loaded && strcmp(layer->path, path) != 0) {
		gs_image_file_free(&layer->image);
		layer->loaded = false;
	}

	/* Load new image/GIF */
	if (path && path[0] && !layer->loaded) {
		strncpy(layer->path, path, sizeof(layer->path) - 1);
		gs_image_file_init(&layer->image, path);

		if (layer->image.loaded) {
			gs_image_file_init_texture(&layer->image);
			layer->loaded = true;
			layer->enabled = true;
			blog(LOG_INFO, "[VJLink] Media layer %d loaded: %s", index, path);
		} else {
			blog(LOG_WARNING, "[VJLink] Failed to load media: %s", path);
		}
	}

	layer->trigger_mode = trigger_mode;
	layer->trigger_band = (trigger_band >= 0 && trigger_band < VJLINK_NUM_BANDS)
		? trigger_band : 0;
	layer->threshold = fmaxf(0.0f, fminf(1.0f, threshold));
	layer->intensity = fmaxf(0.0f, fminf(2.0f, intensity));
}

void vjlink_media_layer_clear(struct vjlink_media_layers *ml, int index)
{
	if (!ml || index < 0 || index >= VJLINK_MAX_MEDIA_LAYERS)
		return;

	struct vjlink_media_layer *layer = &ml->layers[index];
	if (layer->loaded) {
		gs_image_file_free(&layer->image);
		layer->loaded = false;
	}
	layer->enabled = false;
	layer->path[0] = '\0';
}

static float compute_media_opacity(struct vjlink_media_layer *layer)
{
	struct vjlink_context *ctx = vjlink_get_context();
	float base = layer->opacity * layer->intensity;

	switch (layer->trigger_mode) {
	case VJLINK_MEDIA_ALWAYS_ON:
		return base;

	case VJLINK_MEDIA_BAND_TRIGGER: {
		float band_val = ctx->bands[layer->trigger_band];
		if (band_val > layer->threshold) {
			float range = 1.0f - layer->threshold;
			float activation = (range > 0.001f)
				? (band_val - layer->threshold) / range
				: 0.0f;
			return base * activation;
		}
		return 0.0f;
	}

	case VJLINK_MEDIA_BEAT_TRIGGER: {
		float envelope = 1.0f - ctx->beat_phase;
		envelope = envelope * envelope; /* quadratic decay */
		if (envelope > layer->threshold)
			return base * envelope;
		return 0.0f;
	}

	case VJLINK_MEDIA_GATE: {
		float band_val = ctx->bands[layer->trigger_band];
		if (band_val > layer->threshold)
			return base;
		return 0.0f;
	}

	default:
		return base;
	}
}

void vjlink_media_layers_render(struct vjlink_media_layers *ml,
                                 uint32_t canvas_w, uint32_t canvas_h)
{
	if (!ml || !ml->initialized)
		return;

	struct vjlink_context *ctx = vjlink_get_context();

	for (int i = 0; i < VJLINK_MAX_MEDIA_LAYERS; i++) {
		struct vjlink_media_layer *layer = &ml->layers[i];
		if (!layer->enabled || !layer->loaded)
			continue;

		/* Update GIF animation */
		gs_image_file_tick(&layer->image, ctx->elapsed_time);
		gs_image_file_update_texture(&layer->image);

		/* Compute audio-reactive opacity */
		layer->current_opacity = compute_media_opacity(layer);
		if (layer->current_opacity <= 0.001f)
			continue;

		gs_texture_t *tex = layer->image.texture;
		if (!tex)
			continue;

		/* Calculate position and size */
		float img_w = (float)gs_texture_get_width(tex) * layer->scale;
		float img_h = (float)gs_texture_get_height(tex) * layer->scale;
		float x = layer->pos_x * (float)canvas_w - img_w * 0.5f;
		float y = layer->pos_y * (float)canvas_h - img_h * 0.5f;

		/* Set blend mode */
		gs_blend_state_push();
		gs_enable_blending(true);

		switch (layer->blend_mode) {
		case VJLINK_BLEND_ADD:
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
			break;
		case VJLINK_BLEND_MULTIPLY:
			gs_blend_function(GS_BLEND_DSTCOLOR, GS_BLEND_ZERO);
			break;
		case VJLINK_BLEND_SCREEN:
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR);
			break;
		default:
			gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
			break;
		}

		/* Draw the media layer */
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *img_param = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(img_param, tex);

		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		gs_matrix_scale3f(layer->scale, layer->scale, 1.0f);

		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(tex, 0,
				(uint32_t)(img_w), (uint32_t)(img_h));
		}

		gs_matrix_pop();
		gs_blend_state_pop();
	}
}
