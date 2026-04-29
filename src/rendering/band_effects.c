#include "band_effects.h"
#include "effect_system.h"
#include "audio/audio_texture.h"
#include <obs-module.h>
#include <string.h>
#include <math.h>

void vjlink_band_effects_init(struct vjlink_band_effects *bfx,
                               uint32_t width, uint32_t height)
{
	if (!bfx)
		return;

	memset(bfx, 0, sizeof(*bfx));
	bfx->width = width;
	bfx->height = height;

	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		bfx->slots[i].render_target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		bfx->slots[i].threshold = 0.3f;
		bfx->slots[i].intensity = 1.0f;
		bfx->slots[i].attack = 0.65f;
		bfx->slots[i].release = 0.18f;
		bfx->slots[i].hold_frames = 6.0f;
		bfx->slots[i].blend_mode = VJLINK_BLEND_ADD;
		bfx->slots[i].blend_alpha = 1.0f;
		bfx->render_order[i] = i; /* default: bass(0) bottom -> treble(3) top */
	}

	bfx->composite_target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	bfx->composite_target2 = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	bfx->initialized = true;
	blog(LOG_INFO, "[VJLink] Band effects initialized (%ux%u)", width, height);
}

void vjlink_band_effects_set_slot_response(struct vjlink_band_effects *bfx,
                                           int band, float attack,
                                           float release, float hold_frames)
{
	if (!bfx || band < 0 || band >= VJLINK_NUM_BANDS)
		return;

	struct vjlink_band_slot *slot = &bfx->slots[band];
	slot->attack = fmaxf(0.01f, fminf(1.0f, attack));
	slot->release = fmaxf(0.01f, fminf(1.0f, release));
	slot->hold_frames = fmaxf(0.0f, fminf(60.0f, hold_frames));
}

void vjlink_band_effects_set_order(struct vjlink_band_effects *bfx,
                                    const int order[VJLINK_NUM_BANDS])
{
	if (!bfx || !order)
		return;
	/* Validate: every band index 0..3 must appear exactly once */
	int seen[VJLINK_NUM_BANDS] = {0};
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		int v = order[i];
		if (v < 0 || v >= VJLINK_NUM_BANDS) return;
		if (seen[v]) return;
		seen[v] = 1;
	}
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		bfx->render_order[i] = order[i];
	}
}

void vjlink_band_effects_destroy(struct vjlink_band_effects *bfx)
{
	if (!bfx)
		return;

	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		if (bfx->slots[i].render_target) {
			gs_texrender_destroy(bfx->slots[i].render_target);
			bfx->slots[i].render_target = NULL;
		}
	}

	if (bfx->composite_target) {
		gs_texrender_destroy(bfx->composite_target);
		bfx->composite_target = NULL;
	}
	if (bfx->composite_target2) {
		gs_texrender_destroy(bfx->composite_target2);
		bfx->composite_target2 = NULL;
	}

	bfx->initialized = false;
}

void vjlink_band_effects_resize(struct vjlink_band_effects *bfx,
                                 uint32_t width, uint32_t height)
{
	if (!bfx)
		return;

	bfx->width = width;
	bfx->height = height;

	/* Recreate render targets */
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		if (bfx->slots[i].render_target)
			gs_texrender_destroy(bfx->slots[i].render_target);
		bfx->slots[i].render_target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	}

	if (bfx->composite_target)
		gs_texrender_destroy(bfx->composite_target);
	bfx->composite_target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	if (bfx->composite_target2)
		gs_texrender_destroy(bfx->composite_target2);
	bfx->composite_target2 = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
}

void vjlink_band_effects_set_slot(struct vjlink_band_effects *bfx,
                                   int band, const char *effect_id,
                                   float threshold, float intensity)
{
	if (!bfx || band < 0 || band >= VJLINK_NUM_BANDS)
		return;

	struct vjlink_band_slot *slot = &bfx->slots[band];

	if (!effect_id || !effect_id[0]) {
		vjlink_band_effects_clear_slot(bfx, band);
		return;
	}

	/* Only reinit params when effect actually changes */
	bool effect_changed = (strcmp(slot->effect_id, effect_id) != 0);

	slot->threshold = fmaxf(0.0f, fminf(1.0f, threshold));
	slot->intensity = fmaxf(0.0f, fminf(2.0f, intensity));

	if (effect_changed) {
		strncpy(slot->effect_id, effect_id, sizeof(slot->effect_id) - 1);
		slot->entry = vjlink_effect_system_find(effect_id);
		slot->enabled = (slot->entry != NULL);

		if (slot->enabled) {
			/* Initialize param values from metadata defaults */
			for (uint32_t i = 0; i < slot->entry->param_count; i++) {
				for (int c = 0; c < 4; c++)
					slot->param_values[i][c] =
						slot->entry->params[i].default_val[c];
			}
		}
	}

	if (slot->enabled) {
		blog(LOG_INFO,
		     "[VJLink] Band %d: effect='%s', threshold=%.2f, intensity=%.2f%s",
		     band, effect_id, threshold, intensity,
		     effect_changed ? " (new)" : "");
	} else {
		blog(LOG_WARNING,
		     "[VJLink] Band %d: effect '%s' not found", band, effect_id);
	}
}

void vjlink_band_effects_clear_slot(struct vjlink_band_effects *bfx, int band)
{
	if (!bfx || band < 0 || band >= VJLINK_NUM_BANDS)
		return;

	struct vjlink_band_slot *slot = &bfx->slots[band];
	slot->effect_id[0] = '\0';
	slot->entry = NULL;
	slot->enabled = false;
	slot->current_activation = 0.0f;
}

void vjlink_band_effects_update(struct vjlink_band_effects *bfx)
{
	if (!bfx)
		return;

	struct vjlink_context *ctx = vjlink_get_context();

	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		struct vjlink_band_slot *slot = &bfx->slots[i];
		if (!slot->enabled) {
			slot->current_activation = 0.0f;
			slot->target_activation = 0.0f;
			slot->hold_remaining = 0.0f;
			continue;
		}

		float band_val = ctx->bands[i];
		float activation = 0.0f;

		if (band_val > slot->threshold) {
			float range = 1.0f - slot->threshold;
			if (range > 0.001f)
				activation = (band_val - slot->threshold) / range;
			activation *= slot->intensity;
			if (activation > 1.0f)
				activation = 1.0f;
		}

		slot->target_activation = activation;
		if (activation > 0.001f) {
			slot->hold_remaining = slot->hold_frames;
		} else if (slot->hold_remaining > 0.0f) {
			slot->hold_remaining -= 1.0f;
			activation = slot->current_activation;
		}

		float rate = (activation > slot->current_activation)
			? slot->attack : slot->release;
		slot->current_activation +=
			(activation - slot->current_activation) * rate;
		if (slot->current_activation < 0.001f)
			slot->current_activation = 0.0f;
	}

	/* Log band activations every ~5 seconds at 60fps (300 frames) */
	{
		if ((bfx->log_counter++ % 300) == 0) {
			for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
				struct vjlink_band_slot *s = &bfx->slots[i];
				if (s->enabled) {
					blog(LOG_INFO,
					     "[VJLink] Band %d: val=%.3f "
					     "thresh=%.3f activation=%.3f "
					     "effect='%s'",
					     i, ctx->bands[i],
					     s->threshold,
					     s->current_activation,
					     s->effect_id);
				}
			}
		}
	}
}

static void render_band_slot(struct vjlink_band_effects *bfx,
                              struct vjlink_band_slot *slot,
                              gs_texture_t *input_tex)
{
	if (!slot->entry || !slot->enabled || slot->current_activation <= 0.001f)
		return;

	if (!vjlink_effect_ensure_loaded(slot->entry))
		return;

	gs_texrender_reset(slot->render_target);
	if (!gs_texrender_begin(slot->render_target, bfx->width, bfx->height))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	gs_ortho(0.0f, (float)bfx->width, 0.0f, (float)bfx->height,
	         -100.0f, 100.0f);
	gs_matrix_identity();

	/* Force opaque writes */
	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	/* Bind standard uniforms (NULL input_tex is handled by
	 * bind_uniforms which sets fallback texture for unbound params) */
	vjlink_effect_bind_uniforms(slot->entry, input_tex, NULL,
	                            bfx->width, bfx->height);

	if (slot->entry->p_has_input)
		gs_effect_set_float(slot->entry->p_has_input,
		                    input_tex ? 1.0f : 0.0f);

	/* Bind band_activation uniform */
	if (slot->entry->p_band_activation)
		gs_effect_set_float(slot->entry->p_band_activation,
		                    slot->current_activation);

	/* Bind custom parameter values (from band slot or defaults) */
	vjlink_effect_bind_custom_params(slot->entry,
		(const float (*)[4])slot->param_values);

	/* Draw full-screen quad */
	while (gs_effect_loop(slot->entry->effect, "Draw")) {
		gs_draw_sprite(NULL, 0, bfx->width, bfx->height);
	}

	gs_blend_state_pop();

	gs_texrender_end(slot->render_target);
}

gs_texture_t *vjlink_band_effects_render(struct vjlink_band_effects *bfx,
                                          gs_texture_t *input_tex)
{
	if (!bfx || !bfx->initialized)
		return input_tex;

	/* Update activation values */
	vjlink_band_effects_update(bfx);

	/* Check if any band has activation > 0 */
	bool any_active = false;
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		if (bfx->slots[i].enabled && bfx->slots[i].current_activation > 0.001f) {
			any_active = true;
			break;
		}
	}

	if (!any_active)
		return input_tex;

	/* Copy input to composite_target as base layer.
	 * If input_tex is NULL (transparent_bg mode), start with
	 * a transparent base so only the flash overlays show. */
	gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *img = gs_effect_get_param_by_name(default_effect, "image");

	gs_texrender_reset(bfx->composite_target);
	if (!gs_texrender_begin(bfx->composite_target, bfx->width, bfx->height))
		return input_tex;

	{
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

		if (input_tex) {
			gs_ortho(0.0f, (float)bfx->width, 0.0f, (float)bfx->height,
			         -1.0f, 1.0f);
			gs_matrix_identity();

			gs_effect_set_texture(img, input_tex);
			while (gs_effect_loop(default_effect, "Draw")) {
				gs_draw_sprite(input_tex, 0, bfx->width, bfx->height);
			}
		}
	}
	gs_texrender_end(bfx->composite_target);

	/* Ping-pong: composite_target has current result,
	 * composite_target2 is the spare buffer.
	 * After each band blend, swap them. */
	gs_texrender_t *src_buf = bfx->composite_target;
	gs_texrender_t *dst_buf = bfx->composite_target2;

	/* Render each active band effect and blend onto composite, in user-defined Z order. */
	for (int oi = 0; oi < VJLINK_NUM_BANDS; oi++) {
		int i = bfx->render_order[oi];
		if (i < 0 || i >= VJLINK_NUM_BANDS) i = oi; /* defensive */
		struct vjlink_band_slot *slot = &bfx->slots[i];
		if (!slot->enabled || slot->current_activation <= 0.001f)
			continue;

		/* Render the effect to its own render target */
		render_band_slot(bfx, slot, input_tex);

		gs_texture_t *band_tex = gs_texrender_get_texture(slot->render_target);
		if (!band_tex)
			continue;

		gs_texture_t *comp_tex = gs_texrender_get_texture(src_buf);
		if (!comp_tex)
			continue;

		/* Blend band effect onto dst_buf */
		gs_texrender_reset(dst_buf);
		if (!gs_texrender_begin(dst_buf, bfx->width, bfx->height))
			continue;

		{
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_ortho(0.0f, (float)bfx->width, 0.0f, (float)bfx->height,
			         -1.0f, 1.0f);
			gs_matrix_identity();

			/* Draw current composite as base */
			gs_effect_set_texture(img, comp_tex);
			while (gs_effect_loop(default_effect, "Draw")) {
				gs_draw_sprite(comp_tex, 0, bfx->width, bfx->height);
			}

			/* Blend band effect on top with activation-scaled alpha */
			gs_blend_state_push();
			gs_enable_blending(true);
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);

			gs_effect_set_texture(img, band_tex);
			while (gs_effect_loop(default_effect, "Draw")) {
				gs_draw_sprite(band_tex, 0, bfx->width, bfx->height);
			}

			gs_blend_state_pop();
		}
		gs_texrender_end(dst_buf);

		/* Swap: dst becomes src for next band */
		gs_texrender_t *tmp = src_buf;
		src_buf = dst_buf;
		dst_buf = tmp;
	}

	return gs_texrender_get_texture(src_buf);
}
