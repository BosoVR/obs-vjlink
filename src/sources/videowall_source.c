#include "videowall_source.h"
#include "vjlink_context.h"
#include <obs-module.h>
#include <graphics/graphics.h>

/*
 * VJLink Video Wall Source
 *
 * Displays a sub-region of the compositor output. Each instance defines
 * a normalized region (0.0 - 1.0) and renders that portion via a
 * UV-remapping blit shader. No CPU readbacks.
 *
 * Usage: Create multiple Video Wall sources in different OBS scenes,
 * each showing a different region of the compositor. Ideal for
 * multi-display / video wall setups.
 */

struct vjlink_videowall_data {
	obs_source_t  *source;
	uint32_t       width;
	uint32_t       height;

	/* Region in compositor output (normalized 0.0 - 1.0) */
	float          region_x;
	float          region_y;
	float          region_w;
	float          region_h;

	/* Blit shader */
	gs_effect_t   *blit_effect;
	bool           initialized;
};

static const char *vjlink_videowall_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("VJLinkVideoWall");
}

static void *vjlink_videowall_create(obs_data_t *settings,
                                      obs_source_t *source)
{
	struct vjlink_videowall_data *wall = calloc(1, sizeof(*wall));
	if (!wall)
		return NULL;

	wall->source = source;
	wall->width = (uint32_t)obs_data_get_int(settings, "width");
	wall->height = (uint32_t)obs_data_get_int(settings, "height");

	if (wall->width == 0) wall->width = 1920;
	if (wall->height == 0) wall->height = 1080;

	wall->region_x = (float)obs_data_get_double(settings, "region_x");
	wall->region_y = (float)obs_data_get_double(settings, "region_y");
	wall->region_w = (float)obs_data_get_double(settings, "region_w");
	wall->region_h = (float)obs_data_get_double(settings, "region_h");

	if (wall->region_w <= 0.0f) wall->region_w = 1.0f;
	if (wall->region_h <= 0.0f) wall->region_h = 1.0f;

	blog(LOG_INFO, "[VJLink] Video Wall source created (region: %.2f,%.2f %.2fx%.2f)",
	     wall->region_x, wall->region_y, wall->region_w, wall->region_h);
	return wall;
}

static void vjlink_videowall_destroy(void *data)
{
	struct vjlink_videowall_data *wall = data;
	if (!wall)
		return;

	obs_enter_graphics();
	if (wall->blit_effect)
		gs_effect_destroy(wall->blit_effect);
	obs_leave_graphics();

	free(wall);
	blog(LOG_INFO, "[VJLink] Video Wall source destroyed");
}

static void vjlink_videowall_update(void *data, obs_data_t *settings)
{
	struct vjlink_videowall_data *wall = data;

	uint32_t w = (uint32_t)obs_data_get_int(settings, "width");
	uint32_t h = (uint32_t)obs_data_get_int(settings, "height");
	if (w > 0) wall->width = w;
	if (h > 0) wall->height = h;

	wall->region_x = (float)obs_data_get_double(settings, "region_x");
	wall->region_y = (float)obs_data_get_double(settings, "region_y");
	wall->region_w = (float)obs_data_get_double(settings, "region_w");
	wall->region_h = (float)obs_data_get_double(settings, "region_h");

	if (wall->region_w <= 0.0f) wall->region_w = 1.0f;
	if (wall->region_h <= 0.0f) wall->region_h = 1.0f;
}

static uint32_t vjlink_videowall_get_width(void *data)
{
	return ((struct vjlink_videowall_data *)data)->width;
}

static uint32_t vjlink_videowall_get_height(void *data)
{
	return ((struct vjlink_videowall_data *)data)->height;
}

static obs_properties_t *vjlink_videowall_properties(void *data)
{
	UNUSED_PARAMETER(data);

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

	/* Region group */
	obs_properties_t *region_group = obs_properties_create();
	obs_properties_add_float_slider(region_group, "region_x",
		obs_module_text("VJLinkVideoWall.RegionX"),
		0.0, 1.0, 0.01);
	obs_properties_add_float_slider(region_group, "region_y",
		obs_module_text("VJLinkVideoWall.RegionY"),
		0.0, 1.0, 0.01);
	obs_properties_add_float_slider(region_group, "region_w",
		obs_module_text("VJLinkVideoWall.RegionW"),
		0.01, 1.0, 0.01);
	obs_properties_add_float_slider(region_group, "region_h",
		obs_module_text("VJLinkVideoWall.RegionH"),
		0.01, 1.0, 0.01);
	obs_properties_add_group(props, "region_group",
		"Compositor Region", OBS_GROUP_NORMAL, region_group);

	return props;
}

static void vjlink_videowall_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_double(settings, "region_x", 0.0);
	obs_data_set_default_double(settings, "region_y", 0.0);
	obs_data_set_default_double(settings, "region_w", 1.0);
	obs_data_set_default_double(settings, "region_h", 1.0);
}

static void vjlink_videowall_video_render(void *data, gs_effect_t *obs_effect)
{
	UNUSED_PARAMETER(obs_effect);
	struct vjlink_videowall_data *wall = data;
	struct vjlink_context *ctx = vjlink_get_context();

	/* Lazy-init blit shader */
	if (!wall->initialized) {
		char *path = obs_module_file("effects/common/videowall_blit.effect");
		if (path) {
			wall->blit_effect = gs_effect_create_from_file(path, NULL);
			bfree(path);
		}
		if (!wall->blit_effect) {
			blog(LOG_WARNING,
			     "[VJLink] Could not load video wall blit shader");
		}
		wall->initialized = true;
	}

	/* Get compositor output texture */
	gs_texture_t *comp_tex = NULL;
	if (ctx->compositor_output)
		comp_tex = gs_texrender_get_texture(ctx->compositor_output);

	if (!comp_tex) {
		/* No compositor output yet - draw black */
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color_param =
			gs_effect_get_param_by_name(solid, "color");
		struct vec4 black;
		vec4_zero(&black);
		black.w = 1.0f;
		gs_effect_set_vec4(color_param, &black);

		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(NULL, 0, wall->width, wall->height);
		return;
	}

	if (wall->blit_effect) {
		/* Use blit shader for UV remapping */
		gs_eparam_t *p;

		p = gs_effect_get_param_by_name(wall->blit_effect, "image");
		if (p) gs_effect_set_texture(p, comp_tex);

		struct vec4 region;
		vec4_set(&region, wall->region_x, wall->region_y,
		         wall->region_w, wall->region_h);
		p = gs_effect_get_param_by_name(wall->blit_effect, "region");
		if (p) gs_effect_set_vec4(p, &region);

		gs_ortho(0.0f, (float)wall->width, 0.0f,
		         (float)wall->height, -1.0f, 1.0f);

		while (gs_effect_loop(wall->blit_effect, "Draw"))
			gs_draw_sprite(comp_tex, 0, wall->width, wall->height);
	} else {
		/* Fallback: draw full compositor output without region crop */
		gs_effect_t *default_effect =
			obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *img =
			gs_effect_get_param_by_name(default_effect, "image");
		gs_effect_set_texture(img, comp_tex);

		while (gs_effect_loop(default_effect, "Draw"))
			gs_draw_sprite(comp_tex, 0, wall->width, wall->height);
	}
}

struct obs_source_info vjlink_videowall_source_info = {
	.id             = "vjlink_videowall_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name       = vjlink_videowall_name,
	.create         = vjlink_videowall_create,
	.destroy        = vjlink_videowall_destroy,
	.update         = vjlink_videowall_update,
	.get_width      = vjlink_videowall_get_width,
	.get_height     = vjlink_videowall_get_height,
	.get_properties = vjlink_videowall_properties,
	.get_defaults   = vjlink_videowall_defaults,
	.video_render   = vjlink_videowall_video_render,
};
