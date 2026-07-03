#include "compositor.h"
#include "effect_system.h"
#include "band_effects.h"
#include "rendering/engine3d.h"
#include "audio/audio_texture.h"
#include <obs-module.h>
#include <graphics/matrix4.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define VJLINK_DEG_TO_RAD 0.01745329251994329577f

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
		comp->chain[i].output = gs_texrender_create(GS_RGBA, GS_Z24_S8);
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

static void bind_internal_effect_defaults(gs_effect_t *effect,
                                          gs_texture_t *fallback_tex)
{
	if (!effect)
		return;

	size_t num_params = gs_effect_get_num_params(effect);
	for (size_t i = 0; i < num_params; i++) {
		gs_eparam_t *param = gs_effect_get_param_by_idx(effect, i);
		struct gs_effect_param_info info;

		gs_effect_get_param_info(param, &info);
		switch (info.type) {
		case GS_SHADER_PARAM_TEXTURE:
			if (fallback_tex)
				gs_effect_set_texture(param, fallback_tex);
			break;
		case GS_SHADER_PARAM_FLOAT:
			gs_effect_set_float(param, 0.0f);
			break;
		case GS_SHADER_PARAM_INT:
			gs_effect_set_int(param, 0);
			break;
		case GS_SHADER_PARAM_BOOL:
			gs_effect_set_bool(param, false);
			break;
		case GS_SHADER_PARAM_VEC2: {
			struct vec2 z;
			vec2_zero(&z);
			gs_effect_set_vec2(param, &z);
			break;
		}
		case GS_SHADER_PARAM_VEC3: {
			struct vec4 z;
			vec4_zero(&z);
			gs_effect_set_val(param, &z, sizeof(float) * 3);
			break;
		}
		case GS_SHADER_PARAM_VEC4: {
			struct vec4 z;
			vec4_zero(&z);
			gs_effect_set_vec4(param, &z);
			break;
		}
		case GS_SHADER_PARAM_MATRIX4X4: {
			struct matrix4 m;
			if (info.name && strcmp(info.name, "ViewProj") == 0)
				gs_matrix_get(&m);
			else
				matrix4_identity(&m);
			gs_effect_set_matrix4(param, &m);
			break;
		}
		default:
			break;
		}
	}
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
	comp->engine3d = vjlink_engine3d_create();

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
	if (comp->engine3d) {
		vjlink_engine3d_destroy(comp->engine3d);
		comp->engine3d = NULL;
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

static float clampf_local(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float node_param_value(const struct vjlink_effect_node *node,
                              const char *param_name, float fallback)
{
	if (!node || !node->entry || !param_name)
		return fallback;

	for (uint32_t i = 0; i < node->entry->param_count; i++) {
		if (strcmp(node->entry->params[i].name, param_name) == 0)
			return node->param_values[i][0];
	}

	return fallback;
}

static bool node_uses_true3d_mesh(const struct vjlink_effect_node *node)
{
	if (!node || !node->entry)
		return false;

	return strcmp(node->entry->id, "logo_extrude_3d") == 0 ||
	       strcmp(node->entry->id, "screen_ring") == 0;
}

static void set_effect_float_by_name(struct vjlink_effect_entry *entry,
                                     const char *name, float value)
{
	if (!entry || !entry->effect || !name)
		return;

	gs_eparam_t *param = gs_effect_get_param_by_name(entry->effect, name);
	if (param)
		gs_effect_set_float(param, value);
}

static void bind_chain_audio_activation(struct vjlink_effect_entry *entry)
{
	if (!entry || !entry->p_band_activation)
		return;

	struct vjlink_context *ctx = vjlink_get_context();
	float max_band = ctx->bands[0];
	for (int b = 1; b < 4; b++) {
		if (ctx->bands[b] > max_band)
			max_band = ctx->bands[b];
	}
	gs_effect_set_float(entry->p_band_activation, max_band);
}

static void bind_mesh_controls(struct vjlink_effect_entry *entry,
                               float kind, float lane, float alpha)
{
	set_effect_float_by_name(entry, "mesh_kind", kind);
	set_effect_float_by_name(entry, "mesh_text_lane", lane);
	set_effect_float_by_name(entry, "mesh_alpha", alpha);
}

static void draw_fullscreen_effect_draw(struct vjlink_effect_entry *entry,
                                        uint32_t width, uint32_t height)
{
	if (!entry || !entry->effect)
		return;

	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
	gs_matrix_identity();
	bind_mesh_controls(entry, 0.0f, 0.0f, 1.0f);

	while (gs_effect_loop(entry->effect, "Draw")) {
		gs_draw_sprite(NULL, 0, width, height);
	}
}

static void draw_mesh_object(struct vjlink_effect_entry *entry,
                             struct vjlink_mesh *mesh,
                             float kind, float lane, float alpha,
                             float x, float y, float z,
                             float sx, float sy, float sz,
                             float rx, float ry, float rz)
{
	if (!entry || !entry->effect || !mesh)
		return;

	bind_mesh_controls(entry, kind, lane, alpha);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate3f(x, y, z);
	gs_matrix_rotaa4f(1.0f, 0.0f, 0.0f, rx);
	gs_matrix_rotaa4f(0.0f, 1.0f, 0.0f, ry);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, rz);
	gs_matrix_scale3f(sx, sy, sz);

	while (gs_effect_loop(entry->effect, "DrawMesh")) {
		vjlink_mesh_draw(mesh);
	}

	gs_matrix_pop();
}

static bool render_effect_node_3d_scene(struct vjlink_compositor *comp,
                                        struct vjlink_effect_node *node,
                                        gs_texture_t *input_tex,
                                        gs_texture_t *prev_tex,
                                        bool has_real_input)
{
	if (!comp || !node || !node->entry || !node->enabled || !comp->engine3d)
		return false;

	if (!vjlink_effect_ensure_loaded(node->entry)) {
		gs_texrender_reset(node->output);
		if (gs_texrender_begin(node->output, comp->width, comp->height)) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &clear_color, 1.0f, 0);
			gs_texrender_end(node->output);
		}
		return true;
	}

	vjlink_engine3d_create_meshes(comp->engine3d);

	gs_texrender_reset(node->output);
	if (!gs_texrender_begin(node->output, comp->width, comp->height))
		return false;

	struct vec4 clear_color;
	float clear_alpha = comp->transparent_bg ? 0.0f : 1.0f;
	vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, clear_alpha);
	gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &clear_color, 1.0f, 0);

	vjlink_effect_bind_uniforms(node->entry, input_tex, prev_tex,
	                            comp->width, comp->height);
	if (node->entry->p_has_input)
		gs_effect_set_float(node->entry->p_has_input,
		                    has_real_input ? 1.0f : 0.0f);
	vjlink_effect_bind_custom_params(node->entry,
		(const float (*)[4])node->param_values);
	bind_chain_audio_activation(node->entry);

	bool is_screen_ring = strcmp(node->entry->id, "screen_ring") == 0;

	/* No-black safety: draw the shader's proven fullscreen path first.
	 * If a GPU/driver rejects the mesh pass, the user still sees a usable
	 * visual instead of a black frame. Meshes are then layered on top. */
	gs_blend_state_push();
	gs_enable_blending(false);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	set_effect_float_by_name(node->entry, "true3d_mode", 0.0f);
	set_effect_float_by_name(node->entry, "mesh_scene_mix", is_screen_ring ? 1.0f : 0.0f);
	draw_fullscreen_effect_draw(node->entry, comp->width, comp->height);
	gs_blend_state_pop();

	struct vjlink_context *ctx = vjlink_get_context();
	float beat = powf(clampf_local(1.0f - ctx->beat_phase, 0.0f, 1.0f), 3.0f);
	float hit = fmaxf(fmaxf(ctx->kick_onset, ctx->snare_onset * 0.55f),
	                  beat * ctx->bands[0]);
	float react = node_param_value(node, "audio_react", 0.8f);
	float bounce_enabled = node_param_value(node, "beat_bounce", 1.0f) > 0.5f;
	float bounce = bounce_enabled ? hit * react : 0.0f;
	float aspect = (float)comp->width / fmaxf((float)comp->height, 1.0f);

	enum gs_cull_mode old_cull = gs_get_cull_mode();
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_set_cull_mode(GS_NEITHER);
	gs_enable_depth_test(true);
	gs_depth_function(GS_LEQUAL);

	gs_perspective(is_screen_ring ? 54.0f : 48.0f, aspect, 0.05f, 100.0f);

	struct vjlink_mesh *cube = vjlink_engine3d_get_mesh(comp->engine3d,
	                                                    VJLINK_MESH_CUBE);

	if (is_screen_ring) {
		float logo_size = node_param_value(node, "logo_size", 0.42f);
		float pulse = node_param_value(node, "beat_pulse", 0.68f);
		float logo_spin_x = node_param_value(node, "logo_spin_x", 0.08f);
		float logo_spin_y = node_param_value(node, "logo_spin_y", 0.28f);
		float logo_spin_z = node_param_value(node, "logo_spin_z", 0.03f);

		draw_mesh_object(node->entry, cube, 1.0f, 0.0f, 1.0f,
		                 0.0f, 0.0f, 2.45f - bounce * 0.35f,
		                 logo_size * 2.0f * (1.0f + bounce * 0.18f),
		                 logo_size * 2.0f * (1.0f + bounce * 0.18f),
		                 0.18f + bounce * 0.18f,
		                 (-12.0f + sinf(ctx->elapsed_time * 0.41f) * 5.0f +
		                  ctx->elapsed_time * logo_spin_x * 60.0f) *
		                 VJLINK_DEG_TO_RAD,
		                 (20.0f + sinf(ctx->elapsed_time * 0.6f) * 8.0f +
		                  ctx->elapsed_time * logo_spin_y * 60.0f) *
		                 VJLINK_DEG_TO_RAD,
		                 (sinf(ctx->elapsed_time * 0.17f) * 4.5f +
		                  ctx->elapsed_time * logo_spin_z * 60.0f) *
		                 VJLINK_DEG_TO_RAD);
	} else {
		float size = node_param_value(node, "size", 0.38f);
		float depth = node_param_value(node, "depth", 0.62f);
		float px = node_param_value(node, "position_x", 0.0f);
		float py = node_param_value(node, "position_y", 0.0f);
		float spin = node_param_value(node, "spin_speed", 0.0f);
		float auto_rotate = node_param_value(node, "auto_rotate", 0.2f);
		float rx = (node_param_value(node, "rotation_x", -18.0f) +
		            sinf(ctx->elapsed_time * 0.31f) * auto_rotate * 10.0f) *
		           VJLINK_DEG_TO_RAD;
		float ry = (node_param_value(node, "rotation_y", 28.0f) +
		            cosf(ctx->elapsed_time * 0.27f) * auto_rotate * 12.0f) *
		           VJLINK_DEG_TO_RAD;
		float rz = (node_param_value(node, "rotation_z", 0.0f) +
		            ctx->elapsed_time * spin * 60.0f) * VJLINK_DEG_TO_RAD;
		float s = size * 3.2f * (1.0f + bounce * 0.16f);
		float d = 0.18f + depth * 0.42f * (1.0f + hit * react * 0.22f);

		draw_mesh_object(node->entry, cube, 1.0f, 0.0f, 1.0f,
		                 px * 2.0f, py * 1.2f, 2.65f - bounce * 0.3f,
		                 s, s, d, rx, ry, rz);
	}

	gs_enable_depth_test(false);
	gs_set_cull_mode(old_cull);

	set_effect_float_by_name(node->entry, "true3d_mode", 0.0f);
	set_effect_float_by_name(node->entry, "mesh_scene_mix", 0.0f);

	gs_blend_state_pop();
	gs_texrender_end(node->output);
	return true;
}

static void render_effect_node(struct vjlink_compositor *comp,
                               struct vjlink_effect_node *node,
                               gs_texture_t *input_tex,
                               gs_texture_t *prev_tex,
                               bool has_real_input)
{
	if (!node->entry || !node->enabled)
		return;

	/* Ensure shader is compiled */
	if (!vjlink_effect_ensure_loaded(node->entry)) {
		/* Avoid showing a stale render target from the previous effect
		 * when a shader fails or is still compiling. */
		gs_texrender_reset(node->output);
		if (gs_texrender_begin(node->output, comp->width, comp->height)) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_texrender_end(node->output);
		}
		return;
	}

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

	/* Dual-mode shaders use this to choose generator or filter mode. */
	if (node->entry->p_has_input)
		gs_effect_set_float(node->entry->p_has_input,
		                    has_real_input ? 1.0f : 0.0f);

	/* Bind custom parameter values */
	vjlink_effect_bind_custom_params(node->entry,
		(const float (*)[4])node->param_values);

	/* For flash/strobe effects in the main chain: auto-set band_activation
	 * from audio so they work without per-band assignment. */
	bind_chain_audio_activation(node->entry);

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

gs_texture_t *vjlink_compositor_render(struct vjlink_compositor *comp,
                                       gs_texture_t *base_tex)
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
	bool has_base = (base_tex != NULL);
	bool has_band_fx = false;
	for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
		if (comp->band_fx.slots[i].enabled) {
			has_band_fx = true;
			break;
		}
	}

	/* Nothing to render: return media base if present, otherwise NULL
	 * (compositor stays transparent). */
	if (!has_chain && !has_band_fx)
		return base_tex;

	/* When transparent_bg is on and only band effects are active (no chain),
	 * use NULL as prev_output so band effects render over transparency
	 * instead of opaque black. */
	gs_texture_t *feedback_tex = vjlink_compositor_get_feedback_tex(comp);
	if (!feedback_tex)
		feedback_tex = comp->seed_tex;
	gs_texture_t *prev_output = NULL;

	if (has_chain) {
		prev_output = has_base ? base_tex : feedback_tex;
	} else {
		/* No chain: band effects start from an empty base.
		 * Never reuse feedback_tex here — it still holds the last
		 * rendered frame from a previously-active effect, which would
		 * make a cleared main effect appear frozen on screen. */
		prev_output = base_tex;
	}

	/* Render each effect in the chain */
	for (uint32_t i = 0; i < comp->chain_length; i++) {
		struct vjlink_effect_node *node = &comp->chain[i];
		if (!node->enabled)
			continue;

		bool node_has_input = has_base || (i > 0);
		if (node_uses_true3d_mesh(node)) {
			if (!render_effect_node_3d_scene(comp, node, prev_output,
			                                 feedback_tex, node_has_input))
				render_effect_node(comp, node, prev_output,
				                   feedback_tex, node_has_input);
		} else {
			render_effect_node(comp, node, prev_output, feedback_tex,
			                   node_has_input);
		}

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

				bind_internal_effect_defaults(comp->luma_alpha_effect,
				                              prev_output);
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

				bind_internal_effect_defaults(comp->debug_effect, prev_output);
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
