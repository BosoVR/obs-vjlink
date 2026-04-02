#include "compositor_source.h"
#include "vjlink_context.h"
#include "rendering/compositor.h"
#include "rendering/effect_system.h"
#include "rendering/media_layer.h"
#include "controls/source_trigger.h"
#include "audio/audio_texture.h"
#include "ui/properties_builder.h"
#include "controls/hotkey_manager.h"
#include <obs-module.h>
#include <graphics/graphics.h>

/*
 * VJLink Compositor Source (Phase 2)
 *
 * The main visual canvas. Uses the compositor renderer with full
 * effect chain support, feedback buffers, and blend modes.
 *
 * Properties:
 *   - width/height: output resolution
 *   - effect: currently selected effect from registry
 */

static const char *band_names[] = {"Bass", "Low-Mid", "High-Mid", "Treble"};
static const char *band_keys[]  = {"bass", "lowmid", "highmid", "treble"};

struct vjlink_compositor_data {
	obs_source_t              *source;
	uint32_t                   width;
	uint32_t                   height;
	struct vjlink_compositor  *renderer;
	char                       active_effect[64];
	bool                       needs_effect_update;
	bool                       needs_band_update;

	/* Logo image */
	char                       logo_path[512];
	gs_image_file_t            logo_image;
	bool                       logo_loaded;
	bool                       logo_needs_load;

	/* Media layers and source triggers */
	struct vjlink_media_layers    media_layers;
	struct vjlink_source_triggers source_triggers;
};

static const char *vjlink_compositor_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("VJLinkCompositor");
}

static void *vjlink_compositor_create(obs_data_t *settings, obs_source_t *source)
{
	struct vjlink_compositor_data *comp = calloc(1, sizeof(*comp));
	if (!comp)
		return NULL;

	comp->source = source;
	comp->width = (uint32_t)obs_data_get_int(settings, "width");
	comp->height = (uint32_t)obs_data_get_int(settings, "height");

	if (comp->width == 0) comp->width = 1920;
	if (comp->height == 0) comp->height = 1080;

	const char *effect = obs_data_get_string(settings, "effect");
	if (effect && *effect)
		strncpy(comp->active_effect, effect, sizeof(comp->active_effect) - 1);

	/* Update global context */
	struct vjlink_context *ctx = vjlink_get_context();
	ctx->compositor_width = comp->width;
	ctx->compositor_height = comp->height;

	comp->needs_effect_update = true;

	/* Initialize media layers and source triggers */
	vjlink_media_layers_init(&comp->media_layers, comp->width, comp->height);
	vjlink_source_triggers_init(&comp->source_triggers);

	/* Set context pointers for WebSocket access */
	ctx->active_media_layers = &comp->media_layers;
	ctx->active_source_triggers = &comp->source_triggers;

	blog(LOG_INFO, "[VJLink] Compositor source created (%ux%u)", comp->width, comp->height);
	return comp;
}

static void vjlink_compositor_destroy(void *data)
{
	struct vjlink_compositor_data *comp = data;
	if (!comp)
		return;

	/* Clear context pointers */
	struct vjlink_context *ctx = vjlink_get_context();
	ctx->active_band_fx = NULL;
	ctx->active_media_layers = NULL;
	ctx->active_source_triggers = NULL;

	obs_enter_graphics();
	if (comp->logo_loaded) {
		gs_image_file_free(&comp->logo_image);
		comp->logo_loaded = false;
	}
	vjlink_media_layers_destroy(&comp->media_layers);
	if (comp->renderer)
		vjlink_compositor_destroy_renderer(comp->renderer);
	obs_leave_graphics();
	ctx->logo_texture = NULL;
	ctx->compositor_output = NULL;

	free(comp);
	blog(LOG_INFO, "[VJLink] Compositor source destroyed");
}

static void vjlink_compositor_update(void *data, obs_data_t *settings)
{
	struct vjlink_compositor_data *comp = data;
	uint32_t new_width = (uint32_t)obs_data_get_int(settings, "width");
	uint32_t new_height = (uint32_t)obs_data_get_int(settings, "height");

	if (new_width > 0 && new_width != comp->width) {
		comp->width = new_width;
		if (comp->renderer)
			vjlink_compositor_resize(comp->renderer, comp->width, comp->height);
	}
	if (new_height > 0 && new_height != comp->height) {
		comp->height = new_height;
		if (comp->renderer)
			vjlink_compositor_resize(comp->renderer, comp->width, comp->height);
	}

	const char *effect = obs_data_get_string(settings, "effect");
	if (effect && strcmp(effect, comp->active_effect) != 0) {
		strncpy(comp->active_effect, effect, sizeof(comp->active_effect) - 1);
		comp->needs_effect_update = true;
	}

	/* Transparent background toggle */
	bool tbg = obs_data_get_bool(settings, "transparent_bg");
	if (comp->renderer)
		comp->renderer->transparent_bg = tbg;

	/* Debug overlay toggle */
	if (comp->renderer)
		comp->renderer->debug_overlay = obs_data_get_bool(settings, "debug_overlay");

	/* Update error display */
	{
		struct vjlink_context *c = vjlink_get_context();
		if (c->has_error)
			obs_data_set_string(settings, "last_error_display", c->last_error);
	}

	/* Logo image path */
	const char *logo = obs_data_get_string(settings, "logo_path");
	if (logo && strcmp(logo, comp->logo_path) != 0) {
		strncpy(comp->logo_path, logo, sizeof(comp->logo_path) - 1);
		comp->logo_needs_load = true;
	}

	/* Read custom effect parameters from OBS UI sliders.
	 * Only read if the key actually exists in settings,
	 * otherwise keep the metadata defaults from chain_add. */
	if (comp->renderer && comp->renderer->chain_length > 0) {
		struct vjlink_effect_node *node = &comp->renderer->chain[0];
		if (node->entry) {
			for (uint32_t i = 0; i < node->entry->param_count; i++) {
				struct vjlink_param_def *p = &node->entry->params[i];
				char prop_id[128];
				snprintf(prop_id, sizeof(prop_id), "ep_%s", p->name);

				/* Skip if key doesn't exist in settings
				 * (preserves defaults and WebSocket-set values) */
				if (!obs_data_has_user_value(settings, prop_id))
					continue;

				switch (p->type) {
				case VJLINK_PARAM_FLOAT:
					node->param_values[i][0] =
						(float)obs_data_get_double(settings, prop_id);
					break;
				case VJLINK_PARAM_INT:
					node->param_values[i][0] =
						(float)obs_data_get_int(settings, prop_id);
					break;
				case VJLINK_PARAM_BOOL:
					node->param_values[i][0] =
						obs_data_get_bool(settings, prop_id)
						? 1.0f : 0.0f;
					break;
				case VJLINK_PARAM_COLOR: {
					long col = (long)obs_data_get_int(settings, prop_id);
					node->param_values[i][0] = ((col >>  0) & 0xFF) / 255.0f;
					node->param_values[i][1] = ((col >>  8) & 0xFF) / 255.0f;
					node->param_values[i][2] = ((col >> 16) & 0xFF) / 255.0f;
					node->param_values[i][3] = ((col >> 24) & 0xFF) / 255.0f;
					break;
				}
				case VJLINK_PARAM_VEC2: {
					char id_x[140], id_y[140];
					snprintf(id_x, sizeof(id_x), "ep_%s_x", p->name);
					snprintf(id_y, sizeof(id_y), "ep_%s_y", p->name);
					node->param_values[i][0] =
						(float)obs_data_get_double(settings, id_x);
					node->param_values[i][1] =
						(float)obs_data_get_double(settings, id_y);
					break;
				}
				case VJLINK_PARAM_VEC4:
					for (int c = 0; c < 4; c++) {
						char id_c[140];
						snprintf(id_c, sizeof(id_c),
						         "ep_%s_%d", p->name, c);
						node->param_values[i][c] =
							(float)obs_data_get_double(settings, id_c);
					}
					break;
				}
			}
		}
	}

	/* Mark band effects for update */
	comp->needs_band_update = true;

	struct vjlink_context *ctx = vjlink_get_context();
	ctx->compositor_width = comp->width;
	ctx->compositor_height = comp->height;
}

static uint32_t vjlink_compositor_get_width(void *data)
{
	return ((struct vjlink_compositor_data *)data)->width;
}

static uint32_t vjlink_compositor_get_height(void *data)
{
	return ((struct vjlink_compositor_data *)data)->height;
}

static obs_properties_t *vjlink_compositor_properties(void *data)
{
	struct vjlink_compositor_data *comp = data;

	obs_properties_t *props = obs_properties_create();

	/* Resolution group */
	obs_properties_t *res_group = obs_properties_create();
	obs_properties_add_int(res_group, "width",
		obs_module_text("VJLinkCompositor.Width"),
		320, 7680, 1);
	obs_properties_add_int(res_group, "height",
		obs_module_text("VJLinkCompositor.Height"),
		240, 4320, 1);
	obs_properties_add_group(props, "resolution_group",
		"Resolution", OBS_GROUP_NORMAL, res_group);

	/* Transparent background toggle */
	obs_properties_add_bool(props, "transparent_bg",
		"Transparent Background");

	/* Debug overlay */
	obs_properties_add_bool(props, "debug_overlay",
		"Debug Overlay (Bands/BPM/FPS)");

	/* Logo image */
	obs_properties_add_path(props, "logo_path", "Logo Image",
		OBS_PATH_FILE,
		"Image Files (*.png *.jpg *.jpeg *.gif *.bmp);;All Files (*.*)",
		NULL);

	/* Effect selection (categorized dropdown) */
	vjlink_props_add_effect_list(props, "effect",
		obs_module_text("VJLinkCompositor.Effect"));

	/* Preset selection */
	vjlink_props_add_preset_list(props, "preset",
		obs_module_text("VJLinkCompositor.Preset"));

	/* Dynamic effect parameters */
	if (comp && comp->active_effect[0])
		vjlink_props_add_effect_params(props, comp->active_effect);

	/* Per-Band Effect Assignment */
	obs_properties_t *band_group = obs_properties_create();
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		char key_effect[64], key_thresh[64], key_intens[64];
		char label[128];

		snprintf(key_effect, sizeof(key_effect), "band_%s_effect", band_keys[i]);
		snprintf(key_thresh, sizeof(key_thresh), "band_%s_threshold", band_keys[i]);
		snprintf(key_intens, sizeof(key_intens), "band_%s_intensity", band_keys[i]);

		snprintf(label, sizeof(label), "%s Effect", band_names[i]);
		vjlink_props_add_effect_list(band_group, key_effect, label);

		snprintf(label, sizeof(label), "%s Threshold", band_names[i]);
		obs_properties_add_float_slider(band_group, key_thresh, label,
			0.0, 1.0, 0.01);

		snprintf(label, sizeof(label), "%s Intensity", band_names[i]);
		obs_properties_add_float_slider(band_group, key_intens, label,
			0.0, 2.0, 0.01);
	}
	obs_properties_add_group(props, "band_effects_group",
		"Per-Band Effects", OBS_GROUP_NORMAL, band_group);

	/* Last error display (read-only info box) */
	{
		struct vjlink_context *ctx = vjlink_get_context();
		if (ctx->has_error && ctx->last_error[0]) {
			obs_properties_add_text(props, "last_error_display",
				ctx->last_error, OBS_TEXT_INFO_WARNING);
		}
	}

	return props;
}

static void vjlink_compositor_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_string(settings, "effect", "");
	obs_data_set_default_bool(settings, "transparent_bg", false);

	/* Band effect defaults */
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		char key[64];
		snprintf(key, sizeof(key), "band_%s_effect", band_keys[i]);
		obs_data_set_default_string(settings, key, "");
		snprintf(key, sizeof(key), "band_%s_threshold", band_keys[i]);
		obs_data_set_default_double(settings, key, 0.3);
		snprintf(key, sizeof(key), "band_%s_intensity", band_keys[i]);
		obs_data_set_default_double(settings, key, 1.0);
	}

	/* Set defaults for all effect params */
	uint32_t count = vjlink_effect_system_get_count();
	for (uint32_t e = 0; e < count; e++) {
		struct vjlink_effect_entry *entry =
			vjlink_effect_system_get_entry(e);
		if (!entry) continue;
		for (uint32_t p = 0; p < entry->param_count; p++) {
			char prop_id[128];
			snprintf(prop_id, sizeof(prop_id), "ep_%s",
			         entry->params[p].name);
			obs_data_set_default_double(settings, prop_id,
				(double)entry->params[p].default_val[0]);
		}
	}
}

static void vjlink_compositor_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(data);
	vjlink_tick_time(seconds);
}

static void vjlink_compositor_video_render(void *data, gs_effect_t *obs_effect)
{
	UNUSED_PARAMETER(obs_effect);
	struct vjlink_compositor_data *comp = data;
	struct vjlink_context *ctx = vjlink_get_context();

	/* Check GPU capabilities once */
	if (!ctx->gpu_checked)
		vjlink_check_gpu_caps();

	/* Lazy-init renderer (must be in graphics context) */
	if (!comp->renderer) {
		comp->renderer = vjlink_compositor_create_renderer(
			comp->width, comp->height);
		comp->needs_effect_update = true;
		comp->needs_band_update = true;

		/* Set context pointer to band effects */
		ctx->active_band_fx = &comp->renderer->band_fx;
	}

	/* Load/reload logo image (GPU texture init must happen on graphics thread) */
	if (comp->logo_needs_load) {
		if (comp->logo_loaded) {
			gs_image_file_free(&comp->logo_image);
			comp->logo_loaded = false;
			ctx->logo_texture = NULL;
		}
		comp->logo_needs_load = false;
		if (comp->logo_path[0]) {
			gs_image_file_init(&comp->logo_image, comp->logo_path);
			if (comp->logo_image.loaded) {
				gs_image_file_init_texture(&comp->logo_image);
				comp->logo_loaded = true;
				blog(LOG_INFO, "[VJLink] Logo loaded: %s", comp->logo_path);
			} else {
				blog(LOG_WARNING, "[VJLink] Failed to load logo: %s",
				     comp->logo_path);
			}
		}
	}

	/* Check for WebSocket transparent_bg override */
	if (ctx->transparent_bg_pending && comp->renderer) {
		ctx->transparent_bg_pending = false;
		comp->renderer->transparent_bg = ctx->pending_transparent_bg;
		/* Also update OBS settings so update() doesn't overwrite */
		obs_data_t *s = obs_source_get_settings(comp->source);
		if (s) {
			obs_data_set_bool(s, "transparent_bg", ctx->pending_transparent_bg);
			obs_data_release(s);
		}
	}

	/* Check for WebSocket logo override */
	if (ctx->logo_pending) {
		ctx->logo_pending = false;
		strncpy(comp->logo_path, ctx->pending_logo_path,
		        sizeof(comp->logo_path) - 1);
		comp->logo_needs_load = true;
	}

	/* Update logo texture for GIF animation + sync to context */
	if (comp->logo_loaded) {
		gs_image_file_tick(&comp->logo_image, 0.016667f);
		gs_image_file_update_texture(&comp->logo_image);
		ctx->logo_texture = comp->logo_image.texture;
	}

	/* Check for WebSocket effect override */
	if (ctx->effect_pending && comp->renderer) {
		ctx->effect_pending = false;
		if (ctx->pending_effect[0]) {
			strncpy(comp->active_effect, ctx->pending_effect,
			        sizeof(comp->active_effect) - 1);
			vjlink_compositor_set_effect(comp->renderer,
				comp->active_effect);
		} else {
			comp->active_effect[0] = '\0';
			vjlink_compositor_chain_clear(comp->renderer);
		}
		comp->needs_effect_update = false;
	}

	/* Update effect selection if changed via OBS properties */
	if (comp->needs_effect_update && comp->renderer) {
		comp->needs_effect_update = false;

		if (comp->active_effect[0]) {
			vjlink_compositor_set_effect(comp->renderer,
				comp->active_effect);
		} else {
			vjlink_compositor_chain_clear(comp->renderer);
		}
	}

	/* Sync active effect ID to global context for UI polling */
	if (strcmp(ctx->active_effect_id, comp->active_effect) != 0)
		strncpy(ctx->active_effect_id, comp->active_effect,
		        sizeof(ctx->active_effect_id) - 1);

	/* Apply pending WebSocket param changes on render thread */
	if (ctx->pending_param_count > 0 && comp->renderer) {
		int count = ctx->pending_param_count;
		ctx->pending_param_count = 0;
		for (int p = 0; p < count; p++) {
			vjlink_compositor_set_chain_param(
				comp->renderer, 0,
				ctx->pending_params[p].name,
				ctx->pending_params[p].value);
		}
	}

	/* Update per-band effect assignments */
	if (comp->needs_band_update && comp->renderer) {
		comp->needs_band_update = false;
		obs_data_t *settings = obs_source_get_settings(comp->source);
		if (settings) {
			for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
				char key[64];
				snprintf(key, sizeof(key), "band_%s_effect", band_keys[i]);
				const char *eff = obs_data_get_string(settings, key);
				snprintf(key, sizeof(key), "band_%s_threshold", band_keys[i]);
				float thresh = (float)obs_data_get_double(settings, key);
				snprintf(key, sizeof(key), "band_%s_intensity", band_keys[i]);
				float intens = (float)obs_data_get_double(settings, key);

				vjlink_band_effects_set_slot(
					&comp->renderer->band_fx, i,
					eff, thresh, intens);
			}
			obs_data_release(settings);
		}
	}

	/* Blackout: render black and skip everything */
	if (vjlink_is_blackout()) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
		struct vec4 black;
		vec4_zero(&black);
		black.w = 1.0f;
		gs_effect_set_vec4(color_param, &black);
		while (gs_effect_loop(solid, "Solid")) {
			gs_draw_sprite(NULL, 0, comp->width, comp->height);
		}
		vjlink_source_triggers_update(&comp->source_triggers);
		return;
	}

	/* Render the effect chain (also handles band effects when chain_length==0) */
	if (comp->renderer) {
		gs_texture_t *output = vjlink_compositor_render(comp->renderer);

		if (output) {
			/* When transparent_bg is on, the luma_alpha post-process
			 * produces premultiplied alpha output. Use ONE/INVSRCALPHA
			 * blend so OBS correctly composites over sources below. */
			if (comp->renderer->transparent_bg) {
				gs_blend_state_push();
				gs_enable_blending(true);
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			}

			/* Draw the compositor output to screen */
			gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *img = gs_effect_get_param_by_name(default_effect, "image");
			gs_effect_set_texture(img, output);

			while (gs_effect_loop(default_effect, "Draw")) {
				gs_draw_sprite(output, 0, comp->width, comp->height);
			}

			if (comp->renderer->transparent_bg)
				gs_blend_state_pop();

			/* Store in global context for video wall sources */
			ctx->compositor_output = comp->renderer->feedback_current
				? comp->renderer->feedback_a
				: comp->renderer->feedback_b;
		} else {
			/* No output: force fully transparent to clear last frame.
			 * Use BLEND_ONE/BLEND_ZERO to directly write alpha=0,
			 * ensuring OBS doesn't keep the cached last frame. */
			gs_blend_state_push();
			gs_enable_blending(true);
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

			gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
			gs_eparam_t *cp = gs_effect_get_param_by_name(solid, "color");
			struct vec4 clear;
			vec4_zero(&clear);
			gs_effect_set_vec4(cp, &clear);
			while (gs_effect_loop(solid, "Solid")) {
				gs_draw_sprite(NULL, 0, comp->width, comp->height);
			}

			gs_blend_state_pop();
			ctx->compositor_output = NULL;
		}
	}

	/* Render media layers (images/GIFs) */
	vjlink_media_layers_render(&comp->media_layers,
	                            comp->width, comp->height);

	/* Update OBS source triggers */
	vjlink_source_triggers_update(&comp->source_triggers);
}

struct obs_source_info vjlink_compositor_source_info = {
	.id             = "vjlink_compositor_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name       = vjlink_compositor_name,
	.create         = vjlink_compositor_create,
	.destroy        = vjlink_compositor_destroy,
	.update         = vjlink_compositor_update,
	.get_width      = vjlink_compositor_get_width,
	.get_height     = vjlink_compositor_get_height,
	.get_properties = vjlink_compositor_properties,
	.get_defaults   = vjlink_compositor_defaults,
	.video_render   = vjlink_compositor_video_render,
	.video_tick     = vjlink_compositor_tick,
};
