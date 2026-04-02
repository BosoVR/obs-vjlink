#include "effect_filter.h"
#include "vjlink_context.h"
#include "rendering/effect_system.h"
#include "audio/audio_texture.h"
#include "ui/properties_builder.h"
#include <obs-module.h>

/*
 * VJLink Effect Filter
 *
 * Applied to any video source. Runs a VJLink shader effect
 * using the source texture as input and the audio texture as a uniform.
 *
 * Rendering pipeline (matches obs-shaderfilter pattern):
 *
 *   Phase 1 - Capture source:
 *     process_filter_begin(OBS_NO_DIRECT_RENDERING)
 *     texrender_begin(input_texrender)
 *       process_filter_end(pass_through)  → source drawn into our texrender
 *     texrender_end(input_texrender)
 *
 *   Phase 2 - Apply VJLink effect:
 *     texrender_begin(output_texrender)
 *       bind_uniforms(source_tex, ...)    → sets image + input_tex + audio
 *       gs_effect_loop(vjlink_effect)     → draws into output texrender
 *     texrender_end(output_texrender)
 *
 *   Phase 3 - Output to OBS:
 *     process_filter_begin(OBS_NO_DIRECT_RENDERING)
 *     set output_tex on pass_through "image"
 *     process_filter_end(pass_through)    → result into OBS pipeline
 */

struct vjlink_effect_filter_data {
	obs_source_t               *source;
	char                        effect_id[64];
	struct vjlink_effect_entry *entry;
	bool                        needs_update;
	gs_texrender_t             *input_texrender;  /* captured source */
	gs_texrender_t             *output_texrender; /* effect output */
	gs_eparam_t                *cached_passthrough_image; /* cached "image" param */

	/* Transparent background: dark areas become see-through */
	bool                        transparent_bg;

	/* Custom parameter values read from OBS settings */
	float                       param_values[VJLINK_MAX_PARAMS][4];
};

static const char *vjlink_effect_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("VJLinkEffectFilter");
}

static void *vjlink_effect_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct vjlink_effect_filter_data *filter = calloc(1, sizeof(*filter));
	if (!filter)
		return NULL;

	filter->source = source;

	const char *effect = obs_data_get_string(settings, "effect");
	if (effect && *effect)
		strncpy(filter->effect_id, effect, sizeof(filter->effect_id) - 1);

	filter->needs_update = true;

	/* Ensure effect system is initialized */
	vjlink_effect_system_init();

	blog(LOG_INFO, "[VJLink] Effect filter created");
	return filter;
}

static void vjlink_effect_filter_destroy(void *data)
{
	struct vjlink_effect_filter_data *filter = data;
	if (!filter)
		return;

	obs_enter_graphics();
	if (filter->input_texrender)
		gs_texrender_destroy(filter->input_texrender);
	if (filter->output_texrender)
		gs_texrender_destroy(filter->output_texrender);
	obs_leave_graphics();

	free(filter);
}

static void read_custom_params(struct vjlink_effect_filter_data *filter,
                               obs_data_t *settings)
{
	if (!filter->entry)
		return;

	for (uint32_t i = 0; i < filter->entry->param_count; i++) {
		struct vjlink_param_def *p = &filter->entry->params[i];
		char prop_id[128];
		snprintf(prop_id, sizeof(prop_id), "ep_%s", p->name);

		/* Only read if user has set this value in OBS UI,
		 * otherwise keep metadata defaults */
		if (!obs_data_has_user_value(settings, prop_id))
			continue;

		switch (p->type) {
		case VJLINK_PARAM_FLOAT:
			filter->param_values[i][0] =
				(float)obs_data_get_double(settings, prop_id);
			break;
		case VJLINK_PARAM_INT:
			filter->param_values[i][0] =
				(float)obs_data_get_int(settings, prop_id);
			break;
		case VJLINK_PARAM_BOOL:
			filter->param_values[i][0] =
				obs_data_get_bool(settings, prop_id) ? 1.0f : 0.0f;
			break;
		case VJLINK_PARAM_COLOR: {
			long col = (long)obs_data_get_int(settings, prop_id);
			filter->param_values[i][0] = ((col >>  0) & 0xFF) / 255.0f;
			filter->param_values[i][1] = ((col >>  8) & 0xFF) / 255.0f;
			filter->param_values[i][2] = ((col >> 16) & 0xFF) / 255.0f;
			filter->param_values[i][3] = ((col >> 24) & 0xFF) / 255.0f;
			break;
		}
		case VJLINK_PARAM_VEC2: {
			char id_x[140], id_y[140];
			snprintf(id_x, sizeof(id_x), "ep_%s_x", p->name);
			snprintf(id_y, sizeof(id_y), "ep_%s_y", p->name);
			filter->param_values[i][0] =
				(float)obs_data_get_double(settings, id_x);
			filter->param_values[i][1] =
				(float)obs_data_get_double(settings, id_y);
			break;
		}
		case VJLINK_PARAM_VEC4: {
			for (int c = 0; c < 4; c++) {
				char id_c[140];
				snprintf(id_c, sizeof(id_c),
				         "ep_%s_%d", p->name, c);
				filter->param_values[i][c] =
					(float)obs_data_get_double(settings, id_c);
			}
			break;
		}
		}
	}
}

static void vjlink_effect_filter_update(void *data, obs_data_t *settings)
{
	struct vjlink_effect_filter_data *filter = data;

	const char *effect = obs_data_get_string(settings, "effect");
	if (effect && strcmp(effect, filter->effect_id) != 0) {
		strncpy(filter->effect_id, effect, sizeof(filter->effect_id) - 1);
		filter->needs_update = true;
	}

	/* Read custom parameter values from UI sliders */
	read_custom_params(filter, settings);

	/* Transparent background toggle */
	filter->transparent_bg = obs_data_get_bool(settings, "transparent_bg");
}

static obs_properties_t *vjlink_effect_filter_properties(void *data)
{
	struct vjlink_effect_filter_data *filter = data;

	obs_properties_t *props = obs_properties_create();

	/* Transparent background toggle */
	obs_properties_add_bool(props, "transparent_bg",
		"Transparent Background");

	/* Categorized effect dropdown */
	vjlink_props_add_effect_list(props, "effect",
		obs_module_text("VJLinkEffectFilter.Effect"));

	/* Dynamic effect parameters */
	if (filter && filter->effect_id[0])
		vjlink_props_add_effect_params(props, filter->effect_id);

	return props;
}

static void vjlink_effect_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "effect", "");
	obs_data_set_default_bool(settings, "transparent_bg", true);

	/* Set defaults for all effect params across all effects.
	 * OBS ignores defaults for keys that don't appear in properties. */
	uint32_t count = vjlink_effect_system_get_count();
	for (uint32_t e = 0; e < count; e++) {
		struct vjlink_effect_entry *entry =
			vjlink_effect_system_get_entry(e);
		if (!entry) continue;
		for (uint32_t i = 0; i < entry->param_count; i++) {
			struct vjlink_param_def *p = &entry->params[i];
			char prop_id[128];
			snprintf(prop_id, sizeof(prop_id), "ep_%s", p->name);
			obs_data_set_default_double(settings, prop_id,
				(double)p->default_val[0]);
		}
	}
}

/* Create or reset a texrender (never allocate per-frame) */
static gs_texrender_t *create_or_reset(gs_texrender_t *tr)
{
	if (!tr)
		tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	else
		gs_texrender_reset(tr);
	return tr;
}

static void vjlink_effect_filter_video_render(void *data, gs_effect_t *obs_effect)
{
	UNUSED_PARAMETER(obs_effect);
	struct vjlink_effect_filter_data *filter = data;
	struct vjlink_context *ctx = vjlink_get_context();
	if (!ctx)
		return;

	/* Resolve effect entry if needed */
	if (filter->needs_update) {
		filter->needs_update = false;
		filter->cached_passthrough_image = NULL;
		if (filter->effect_id[0]) {
			filter->entry = vjlink_effect_system_find(filter->effect_id);
			if (filter->entry) {
				vjlink_effect_ensure_loaded(filter->entry);
				/* Initialize param values to defaults */
				for (uint32_t i = 0; i < filter->entry->param_count; i++)
					memcpy(filter->param_values[i],
					       filter->entry->params[i].default_val,
					       sizeof(float) * 4);
				/* Then read current settings */
				obs_data_t *s = obs_source_get_settings(filter->source);
				if (s) {
					read_custom_params(filter, s);
					obs_data_release(s);
				}
			}
		} else {
			filter->entry = NULL;
		}
	}

	/* If no effect selected, pass through */
	if (!filter->entry || !filter->entry->effect) {
		obs_source_skip_video_filter(filter->source);
		return;
	}

	/* Get source dimensions */
	obs_source_t *target = obs_filter_get_target(filter->source);
	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);

	if (width == 0 || height == 0) {
		obs_source_skip_video_filter(filter->source);
		return;
	}

	/* Ensure audio texture is uploaded */
	if (!ctx->audio_texture_created)
		vjlink_audio_texture_create();
	vjlink_audio_texture_upload();

	/*
	 * ===== Phase 1: Capture source into input_texrender =====
	 * obs_source_process_filter_begin captures the source.
	 * OBS_NO_DIRECT_RENDERING forces a texrender internally,
	 * guaranteeing "image" gets properly bound.
	 */
	if (!obs_source_process_filter_begin(filter->source, GS_RGBA,
	                                     OBS_NO_DIRECT_RENDERING)) {
		return;
	}

	filter->input_texrender = create_or_reset(filter->input_texrender);

	if (gs_texrender_begin(filter->input_texrender, width, height)) {
		gs_blend_state_push();
		gs_reset_blend_state();
		gs_enable_blending(false);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		gs_ortho(0.0f, (float)width, 0.0f, (float)height,
		         -100.0f, 100.0f);

		/* Draw source into our texrender via pass-through */
		obs_source_process_filter_end(filter->source,
			obs_get_base_effect(OBS_EFFECT_DEFAULT),
			width, height);

		gs_blend_state_pop();
		gs_texrender_end(filter->input_texrender);
	} else {
		/* texrender failed - fall back to simple pass-through */
		obs_source_process_filter_end(filter->source,
			obs_get_base_effect(OBS_EFFECT_DEFAULT),
			width, height);
		return;
	}

	gs_texture_t *source_tex =
		gs_texrender_get_texture(filter->input_texrender);
	if (!source_tex)
		return;

	/*
	 * ===== Phase 2: Apply VJLink effect into output_texrender =====
	 */
	filter->output_texrender = create_or_reset(filter->output_texrender);

	if (!gs_texrender_begin(filter->output_texrender, width, height))
		return;

	{
		struct vec4 clear;
		float clear_alpha = filter->transparent_bg ? 0.0f : 1.0f;
		vec4_set(&clear, 0.0f, 0.0f, 0.0f, clear_alpha);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);

		gs_ortho(0.0f, (float)width, 0.0f, (float)height,
		         -100.0f, 100.0f);

		gs_blend_state_push();
		gs_reset_blend_state();
		gs_enable_blending(false);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		/* Bind source_tex to BOTH "image" and "input_tex" */
		vjlink_effect_bind_uniforms(filter->entry, source_tex, NULL,
		                             width, height);
		vjlink_effect_bind_custom_params(filter->entry,
			(const float (*)[4])filter->param_values);

		/* Tell dual-mode shaders we have real input (filter mode) */
		if (filter->entry->p_has_input)
			gs_effect_set_float(filter->entry->p_has_input, 1.0f);

		while (gs_effect_loop(filter->entry->effect, "Draw")) {
			gs_draw_sprite(source_tex, 0, width, height);
		}

		gs_blend_state_pop();
	}

	gs_texrender_end(filter->output_texrender);

	gs_texture_t *output_tex =
		gs_texrender_get_texture(filter->output_texrender);
	if (!output_tex)
		return;

	/*
	 * ===== Phase 3: Output to OBS pipeline =====
	 * Second process_filter_begin/end pair to properly emit
	 * our result through the OBS filter pipeline.
	 * This handles alpha, color space, and compositing correctly.
	 */
	if (!obs_source_process_filter_begin(filter->source, GS_RGBA,
	                                     OBS_NO_DIRECT_RENDERING)) {
		return;
	}

	/* Bind our output texture to the pass-through effect's "image" */
	gs_effect_t *pass_through = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (!filter->cached_passthrough_image)
		filter->cached_passthrough_image =
			gs_effect_get_param_by_name(pass_through, "image");
	if (filter->cached_passthrough_image)
		gs_effect_set_texture(filter->cached_passthrough_image, output_tex);

	obs_source_process_filter_end(filter->source, pass_through,
	                               width, height);
}

struct obs_source_info vjlink_effect_filter_info = {
	.id             = "vjlink_effect_filter",
	.type           = OBS_SOURCE_TYPE_FILTER,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name       = vjlink_effect_filter_name,
	.create         = vjlink_effect_filter_create,
	.destroy        = vjlink_effect_filter_destroy,
	.update         = vjlink_effect_filter_update,
	.get_properties = vjlink_effect_filter_properties,
	.get_defaults   = vjlink_effect_filter_defaults,
	.video_render   = vjlink_effect_filter_video_render,
};
