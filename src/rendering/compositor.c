#include "compositor.h"
#include "effect_system.h"
#include "band_effects.h"
#include "audio/audio_texture.h"
#include <obs-module.h>
#include <string.h>
#include <stdlib.h>

static void create_feedback_buffers(struct vjlink_compositor *comp)
{
	if (comp->feedback_a)
		gs_texrender_destroy(comp->feedback_a);
	if (comp->feedback_b)
		gs_texrender_destroy(comp->feedback_b);

	comp->feedback_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	comp->feedback_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	comp->feedback_current = false;
}

static void create_chain_render_targets(struct vjlink_compositor *comp)
{
	for (uint32_t i = 0; i < VJLINK_MAX_CHAIN; i++) {
		if (comp->chain[i].output)
			gs_texrender_destroy(comp->chain[i].output);
		comp->chain[i].output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	}
}

static gs_texture_t *create_opaque_black_tex(uint32_t width, uint32_t height)
{
	size_t pixels = (size_t)width * height;
	uint32_t *data = calloc(pixels, sizeof(uint32_t));
	if (!data)
		return NULL;

	for (size_t i = 0; i < pixels; i++)
		data[i] = 0xFF000000;

	const uint8_t *ptr = (const uint8_t *)data;
	gs_texture_t *tex = gs_texture_create(width, height, GS_RGBA, 1, &ptr, 0);
	free(data);
	return tex;
}

struct vjlink_compositor *vjlink_compositor_create_renderer(uint32_t width,
                                                            uint32_t height)
{
	struct vjlink_compositor *comp = calloc(1, sizeof(*comp));
	if (!comp)
		return NULL;

	comp->width = width;
	comp->height = height;
	comp->chain_length = 0;

	/* Initialize effect system if needed */
	vjlink_effect_system_init();

	/* Create GPU resources (must be in graphics context) */
	create_feedback_buffers(comp);
	create_chain_render_targets(comp);

	/* Seed texture: opaque black at full resolution.
	 * Used as input_tex for filter effects before feedback has content. */
	comp->seed_tex = create_opaque_black_tex(width, height);

	/* Initialize per-band effect system */
	vjlink_band_effects_init(&comp->band_fx, width, height);

	comp->initialized = true;
	blog(LOG_INFO, "[VJLink] Compositor renderer created (%ux%u)", width, height);
	return comp;
}

void vjlink_compositor_destroy_renderer(struct vjlink_compositor *comp)
{
	if (!comp)
		return;

	for (uint32_t i = 0; i < VJLINK_MAX_CHAIN; i++) {
		if (comp->chain[i].output) {
			gs_texrender_destroy(comp->chain[i].output);
			comp->chain[i].output = NULL;
		}
	}

	if (comp->feedback_a) {
		gs_texrender_destroy(comp->feedback_a);
		comp->feedback_a = NULL;
	}
	if (comp->feedback_b) {
		gs_texrender_destroy(comp->feedback_b);
		comp->feedback_b = NULL;
	}
	if (comp->blend_effect) {
		gs_effect_destroy(comp->blend_effect);
		comp->blend_effect = NULL;
	}
	if (comp->seed_tex) {
		gs_texture_destroy(comp->seed_tex);
		comp->seed_tex = NULL;
	}
	if (comp->luma_alpha_effect) {
		gs_effect_destroy(comp->luma_alpha_effect);
		comp->luma_alpha_effect = NULL;
	}
	if (comp->luma_alpha_target) {
		gs_texrender_destroy(comp->luma_alpha_target);
		comp->luma_alpha_target = NULL;
	}
	if (comp->debug_effect) {
		gs_effect_destroy(comp->debug_effect);
		comp->debug_effect = NULL;
	}
	if (comp->debug_target) {
		gs_texrender_destroy(comp->debug_target);
		comp->debug_target = NULL;
	}

	vjlink_band_effects_destroy(&comp->band_fx);

	free(comp);
	blog(LOG_INFO, "[VJLink] Compositor renderer destroyed");
}

void vjlink_compositor_resize(struct vjlink_compositor *comp,
                              uint32_t width, uint32_t height)
{
	if (!comp || (comp->width == width && comp->height == height))
		return;

	comp->width = width;
	comp->height = height;

	/* Recreate render targets at new size */
	create_feedback_buffers(comp);
	create_chain_render_targets(comp);

	/* Recreate seed texture at new size */
	if (comp->seed_tex) {
		gs_texture_destroy(comp->seed_tex);
		comp->seed_tex = NULL;
	}
	comp->seed_tex = create_opaque_black_tex(width, height);

	vjlink_band_effects_resize(&comp->band_fx, width, height);

	blog(LOG_INFO, "[VJLink] Compositor resized to %ux%u", width, height);
}

void vjlink_compositor_set_effect(struct vjlink_compositor *comp,
                                  const char *effect_id)
{
	if (!comp)
		return;

	vjlink_compositor_chain_clear(comp);
	vjlink_compositor_chain_add(comp, effect_id, VJLINK_BLEND_NORMAL, 1.0f);
}

bool vjlink_compositor_chain_add(struct vjlink_compositor *comp,
                                 const char *effect_id,
                                 enum vjlink_blend_mode blend_mode,
                                 float blend_alpha)
{
	if (!comp || comp->chain_length >= VJLINK_MAX_CHAIN)
		return false;

	struct vjlink_effect_entry *entry = vjlink_effect_system_find(effect_id);
	if (!entry) {
		blog(LOG_WARNING, "[VJLink] Effect '%s' not found in registry", effect_id);
		return false;
	}

	struct vjlink_effect_node *node = &comp->chain[comp->chain_length];
	strncpy(node->effect_id, effect_id, sizeof(node->effect_id) - 1);
	node->entry = entry;
	node->enabled = true;
	node->blend_alpha = blend_alpha;
	node->blend_mode = blend_mode;

	/* Set default parameter values */
	for (uint32_t i = 0; i < entry->param_count; i++) {
		memcpy(node->param_values[i], entry->params[i].default_val,
		       sizeof(float) * 4);
	}

	comp->chain_length++;
	blog(LOG_INFO, "[VJLink] Added effect '%s' to chain (index %u)",
	     effect_id, comp->chain_length - 1);
	return true;
}

void vjlink_compositor_chain_clear(struct vjlink_compositor *comp)
{
	if (!comp)
		return;

	for (uint32_t i = 0; i < comp->chain_length; i++) {
		comp->chain[i].entry = NULL;
		comp->chain[i].enabled = false;
		memset(comp->chain[i].effect_id, 0, sizeof(comp->chain[i].effect_id));
	}
	comp->chain_length = 0;
}

void vjlink_compositor_set_chain_param(struct vjlink_compositor *comp,
                                       uint32_t chain_index,
                                       const char *param_name,
                                       float value)
{
	if (!comp || !param_name)
		return;

	if (chain_index >= comp->chain_length) {
		blog(LOG_DEBUG, "[VJLink] SetParam '%s'=%.3f DROPPED: "
		     "chain_index=%u >= chain_length=%u",
		     param_name, value, chain_index, comp->chain_length);
		return;
	}

	struct vjlink_effect_node *node = &comp->chain[chain_index];
	if (!node->entry) {
		blog(LOG_DEBUG, "[VJLink] SetParam '%s' DROPPED: no entry",
		     param_name);
		return;
	}

	for (uint32_t i = 0; i < node->entry->param_count; i++) {
		if (strcmp(node->entry->params[i].name, param_name) == 0) {
			node->param_values[i][0] = value;
			blog(LOG_DEBUG, "[VJLink] SetParam '%s'=%.3f OK (idx=%u)",
			     param_name, value, i);
			return;
		}
	}

	blog(LOG_DEBUG, "[VJLink] SetParam '%s'=%.3f NO MATCH in '%s' "
	     "(param_count=%u)",
	     param_name, value, node->entry->id, node->entry->param_count);
}

static void render_effect_node(struct vjlink_compositor *comp,
                               struct vjlink_effect_node *node,
                               gs_texture_t *input_tex,
                               gs_texture_t *prev_tex)
{
	if (!node->entry || !node->enabled)
		return;

	/* Ensure shader is compiled */
	if (!vjlink_effect_ensure_loaded(node->entry))
		return;

	/* Begin rendering to this node's render target */
	gs_texrender_reset(node->output);
	if (!gs_texrender_begin(node->output, comp->width, comp->height))
		return;

	/* Clear render target.
	 * When transparent_bg is on, clear to alpha=0 so dark areas
	 * become see-through (other OBS sources show behind). */
	struct vec4 clear_color;
	float clear_alpha = comp->transparent_bg ? 0.0f : 1.0f;
	vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, clear_alpha);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	/* Set up orthographic projection */
	gs_ortho(0.0f, (float)comp->width, 0.0f, (float)comp->height,
	         -100.0f, 100.0f);
	gs_matrix_identity();

	/* No blending inside texrender - direct write (shader controls alpha) */
	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	/* Bind standard uniforms */
	vjlink_effect_bind_uniforms(node->entry, input_tex, prev_tex,
	                            comp->width, comp->height);

	/* Bind custom parameter values */
	vjlink_effect_bind_custom_params(node->entry,
		(const float (*)[4])node->param_values);

	/* For flash/strobe effects in the main chain: auto-set band_activation
	 * from audio so they work without per-band assignment.
	 * Use max across all bands for broadest trigger. */
	if (node->entry->p_band_activation) {
		struct vjlink_context *ctx = vjlink_get_context();
		float max_band = ctx->bands[0];
		for (int b = 1; b < 4; b++) {
			if (ctx->bands[b] > max_band)
				max_band = ctx->bands[b];
		}
		gs_effect_set_float(node->entry->p_band_activation, max_band);
	}

	/* Draw full-screen quad with custom effect */
	while (gs_effect_loop(node->entry->effect, "Draw")) {
		gs_draw_sprite(NULL, 0, comp->width, comp->height);
	}

	gs_blend_state_pop();

	gs_texrender_end(node->output);
}

static void apply_blend_mode(enum vjlink_blend_mode mode)
{
	switch (mode) {
	case VJLINK_BLEND_ADD:
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		break;
	case VJLINK_BLEND_MULTIPLY:
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_DSTCOLOR, GS_BLEND_ZERO);
		break;
	case VJLINK_BLEND_SCREEN:
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR);
		break;
	case VJLINK_BLEND_NORMAL:
	default:
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
		break;
	}
}

gs_texture_t *vjlink_compositor_get_feedback_tex(struct vjlink_compositor *comp)
{
	if (!comp)
		return NULL;

	/* Return the "previous" feedback buffer's texture */
	gs_texrender_t *prev_buf = comp->feedback_current
		? comp->feedback_a : comp->feedback_b;
	return gs_texrender_get_texture(prev_buf);
}

gs_texture_t *vjlink_compositor_render(struct vjlink_compositor *comp)
{
	if (!comp)
		return NULL;

	struct vjlink_context *ctx = vjlink_get_context();

	/* Ensure audio texture is uploaded */
	if (!ctx->audio_texture_created)
		vjlink_audio_texture_create();
	vjlink_audio_texture_upload();

	/* Hot-reload check: scan active effects every ~120 frames */
	{
		static volatile long reload_counter = 0;
		long count = ++reload_counter;
		if (count >= 120) {
			reload_counter = 0;
			for (uint32_t i = 0; i < comp->chain_length; i++) {
				if (comp->chain[i].entry)
					vjlink_effect_check_hot_reload(comp->chain[i].entry);
			}
			for (int b = 0; b < VJLINK_NUM_BANDS; b++) {
				if (comp->band_fx.slots[b].entry)
					vjlink_effect_check_hot_reload(comp->band_fx.slots[b].entry);
			}
		}
	}

	/* Check if there's anything to render */
	bool has_chain = comp->chain_length > 0;
	bool has_band_fx = false;
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		if (comp->band_fx.slots[i].enabled) {
			has_band_fx = true;
			break;
		}
	}

	/* Nothing to render: return NULL (compositor stays transparent) */
	if (!has_chain && !has_band_fx)
		return NULL;

	/* When transparent_bg is on and only band effects are active (no chain),
	 * use NULL as prev_output so band effects render over transparency
	 * instead of opaque black. */
	gs_texture_t *feedback_tex = vjlink_compositor_get_feedback_tex(comp);
	if (!feedback_tex)
		feedback_tex = comp->seed_tex;
	gs_texture_t *prev_output = NULL;

	if (has_chain) {
		/* Chain effects need input: use feedback or seed */
		prev_output = feedback_tex;
	} else if (!comp->transparent_bg) {
		/* No chain, opaque mode: band effects overlay on black */
		prev_output = feedback_tex;
	}
	/* else: transparent_bg + no chain → prev_output stays NULL */

	/* Render each effect in the chain */
	for (uint32_t i = 0; i < comp->chain_length; i++) {
		struct vjlink_effect_node *node = &comp->chain[i];
		if (!node->enabled)
			continue;

		render_effect_node(comp, node, prev_output, feedback_tex);

		prev_output = gs_texrender_get_texture(node->output);
	}

	/* Copy chain output to feedback buffer BEFORE band effects.
	 * Band effects are ephemeral overlays (flash/strobe) that should
	 * NOT persist in the feedback loop, otherwise they accumulate.
	 * Only copy if chain rendered something (prev_output != NULL). */
	if (prev_output && has_chain) {
		gs_texrender_t *current_fb = comp->feedback_current
			? comp->feedback_b : comp->feedback_a;

		gs_texrender_reset(current_fb);
		if (gs_texrender_begin(current_fb, comp->width, comp->height)) {
			struct vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);

			gs_ortho(0.0f, (float)comp->width, 0.0f,
			         (float)comp->height, -1.0f, 1.0f);
			gs_matrix_identity();

			gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			if (!comp->cached_default_image)
				comp->cached_default_image =
					gs_effect_get_param_by_name(default_effect, "image");
			gs_effect_set_texture(comp->cached_default_image, prev_output);

			while (gs_effect_loop(default_effect, "Draw")) {
				gs_draw_sprite(prev_output, 0,
				               comp->width, comp->height);
			}

			gs_texrender_end(current_fb);
		}

		/* Swap feedback buffers */
		comp->feedback_current = !comp->feedback_current;
	}

	/* Render per-band effects (flash/strobe overlays) AFTER feedback copy.
	 * These are ephemeral: visible only while audio exceeds threshold,
	 * they don't feed back into next frame.
	 * When transparent_bg is on and no chain, prev_output may be NULL -
	 * band_effects_render handles this by starting from transparent. */
	prev_output = vjlink_band_effects_render(&comp->band_fx, prev_output);

	/* Luma-to-alpha post-process: when transparent_bg is on, convert
	 * opaque generative output to proper alpha (dark areas → transparent).
	 * This fixes ALL effects at once without modifying each shader. */
	if (comp->transparent_bg && prev_output) {
		/* Lazy-load the luma_alpha effect */
		if (!comp->luma_alpha_effect) {
			char *path = obs_module_file("effects/common/luma_alpha.effect");
			if (path) {
				char *errors = NULL;
				comp->luma_alpha_effect = gs_effect_create_from_file(path, &errors);
				if (comp->luma_alpha_effect) {
					comp->luma_alpha_image = gs_effect_get_param_by_name(
						comp->luma_alpha_effect, "image");
					blog(LOG_INFO, "[VJLink] luma_alpha effect loaded");
				} else {
					blog(LOG_ERROR, "[VJLink] Failed to load luma_alpha: %s",
					     errors ? errors : "unknown");
				}
				bfree(errors);
				bfree(path);
			}
			if (!comp->luma_alpha_target)
				comp->luma_alpha_target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		}

		if (comp->luma_alpha_effect && comp->luma_alpha_target) {
			gs_texrender_reset(comp->luma_alpha_target);
			if (gs_texrender_begin(comp->luma_alpha_target, comp->width, comp->height)) {
				struct vec4 clear;
				vec4_zero(&clear);
				gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
				gs_ortho(0.0f, (float)comp->width, 0.0f,
				         (float)comp->height, -1.0f, 1.0f);
				gs_matrix_identity();

				gs_blend_state_push();
				gs_enable_blending(false);

				gs_effect_set_texture(comp->luma_alpha_image, prev_output);
				while (gs_effect_loop(comp->luma_alpha_effect, "Draw")) {
					gs_draw_sprite(prev_output, 0, comp->width, comp->height);
				}

				gs_blend_state_pop();
				gs_texrender_end(comp->luma_alpha_target);

				prev_output = gs_texrender_get_texture(comp->luma_alpha_target);
			}
		}
	}

	/* Debug overlay: band bars, beat indicator, BPM, FPS */
	if (comp->debug_overlay && prev_output) {
		/* Lazy-load debug effect */
		if (!comp->debug_effect) {
			char *path = obs_module_file("effects/common/debug_overlay.effect");
			if (path) {
				char *errors = NULL;
				comp->debug_effect = gs_effect_create_from_file(path, &errors);
				if (comp->debug_effect) {
					comp->debug_image      = gs_effect_get_param_by_name(comp->debug_effect, "image");
					comp->debug_resolution = gs_effect_get_param_by_name(comp->debug_effect, "resolution");
					comp->debug_bands      = gs_effect_get_param_by_name(comp->debug_effect, "bands");
					comp->debug_beat_phase = gs_effect_get_param_by_name(comp->debug_effect, "beat_phase");
					comp->debug_bpm        = gs_effect_get_param_by_name(comp->debug_effect, "bpm");
					comp->debug_time       = gs_effect_get_param_by_name(comp->debug_effect, "time");
					comp->debug_fps        = gs_effect_get_param_by_name(comp->debug_effect, "fps");
					comp->debug_onset      = gs_effect_get_param_by_name(comp->debug_effect, "onset_strength");
					blog(LOG_INFO, "[VJLink] Debug overlay effect loaded");
				} else {
					blog(LOG_ERROR, "[VJLink] Failed to load debug overlay: %s",
					     errors ? errors : "unknown");
				}
				bfree(errors);
				bfree(path);
			}
			if (!comp->debug_target)
				comp->debug_target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		}

		if (comp->debug_effect && comp->debug_target) {
			struct vjlink_context *ctx = vjlink_get_context();

			/* FPS counter */
			comp->frame_count += 1.0f;
			comp->fps_timer += 0.016667f; /* ~60fps estimate */
			if (comp->fps_timer >= 1.0f) {
				comp->current_fps = comp->frame_count / comp->fps_timer;
				comp->frame_count = 0.0f;
				comp->fps_timer = 0.0f;
			}

			gs_texrender_reset(comp->debug_target);
			if (gs_texrender_begin(comp->debug_target, comp->width, comp->height)) {
				struct vec4 clear;
				vec4_zero(&clear);
				gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
				gs_ortho(0.0f, (float)comp->width, 0.0f,
				         (float)comp->height, -1.0f, 1.0f);
				gs_matrix_identity();

				gs_blend_state_push();
				gs_enable_blending(false);

				gs_effect_set_texture(comp->debug_image, prev_output);
				if (comp->debug_resolution) {
					struct vec2 res;
					vec2_set(&res, (float)comp->width, (float)comp->height);
					gs_effect_set_vec2(comp->debug_resolution, &res);
				}
				if (comp->debug_bands) {
					struct vec4 b;
					vec4_set(&b, ctx->bands[0], ctx->bands[1],
					         ctx->bands[2], ctx->bands[3]);
					gs_effect_set_vec4(comp->debug_bands, &b);
				}
				if (comp->debug_beat_phase)
					gs_effect_set_float(comp->debug_beat_phase, ctx->beat_phase);
				if (comp->debug_bpm)
					gs_effect_set_float(comp->debug_bpm, ctx->bpm);
				if (comp->debug_time)
					gs_effect_set_float(comp->debug_time, ctx->elapsed_time);
				if (comp->debug_fps)
					gs_effect_set_float(comp->debug_fps, comp->current_fps);
				if (comp->debug_onset)
					gs_effect_set_float(comp->debug_onset, ctx->onset_strength);

				while (gs_effect_loop(comp->debug_effect, "Draw")) {
					gs_draw_sprite(prev_output, 0, comp->width, comp->height);
				}

				gs_blend_state_pop();
				gs_texrender_end(comp->debug_target);
				prev_output = gs_texrender_get_texture(comp->debug_target);
			}
		}
	}

	return prev_output;
}
