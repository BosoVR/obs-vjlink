
#include "particles.h"
#include "audio/audio_texture.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <obs-module.h>

static float randf(void) { return (float)rand() / (float)RAND_MAX; }
static float randf_range(float lo, float hi) { return lo + randf() * (hi - lo); }

struct vjlink_particles *vjlink_particles_create(uint32_t render_width,
                                                  uint32_t render_height)
{
	struct vjlink_particles *ps = calloc(1, sizeof(*ps));
	if (!ps)
		return NULL;

	ps->width = render_width;
	ps->height = render_height;

	/* Create state ping-pong textures */
	ps->state_a = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	ps->state_b = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	ps->vel_a = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	ps->vel_b = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	ps->current_is_a = true;

	/* Output render target */
	ps->output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	/* Load simulation shader */
	char *sim_path = obs_module_file("effects/particles/particle_sim.effect");
	if (sim_path) {
		ps->sim_effect = gs_effect_create_from_file(sim_path, NULL);
		bfree(sim_path);
	}
	if (!ps->sim_effect)
		blog(LOG_WARNING, "[VJLink] Could not load particle sim shader");

	/* Load render shader */
	char *render_path = obs_module_file("effects/particles/particle_render.effect");
	if (render_path) {
		ps->render_effect = gs_effect_create_from_file(render_path, NULL);
		bfree(render_path);
	}
	if (!ps->render_effect)
		blog(LOG_WARNING, "[VJLink] Could not load particle render shader");

	/* Default emitter config */
	ps->config.shape = VJLINK_EMIT_POINT;
	ps->config.radius = 0.5f;
	ps->config.emit_speed = 1.0f;
	ps->config.spread = 0.8f;
	ps->config.lifetime = 3.0f;
	ps->config.gravity = -0.5f;
	ps->config.size_start = 0.02f;
	ps->config.size_end = 0.005f;
	ps->config.color_start[0] = 1.0f;
	ps->config.color_start[1] = 0.8f;
	ps->config.color_start[2] = 0.3f;
	ps->config.color_start[3] = 1.0f;
	ps->config.color_end[0] = 1.0f;
	ps->config.color_end[1] = 0.2f;
	ps->config.color_end[2] = 0.05f;
	ps->config.color_end[3] = 0.0f;
	ps->config.audio_reactive = true;
	ps->config.audio_force_scale = 3.0f;

	ps->initialized = true;
	blog(LOG_INFO, "[VJLink] Particle system created (%u particles)",
	     VJLINK_PARTICLE_COUNT);
	return ps;
}

void vjlink_particles_destroy(struct vjlink_particles *ps)
{
	if (!ps)
		return;

	if (ps->state_a) gs_texrender_destroy(ps->state_a);
	if (ps->state_b) gs_texrender_destroy(ps->state_b);
	if (ps->vel_a) gs_texrender_destroy(ps->vel_a);
	if (ps->vel_b) gs_texrender_destroy(ps->vel_b);
	if (ps->output) gs_texrender_destroy(ps->output);
	if (ps->sim_effect) gs_effect_destroy(ps->sim_effect);
	if (ps->render_effect) gs_effect_destroy(ps->render_effect);

	free(ps);
	blog(LOG_INFO, "[VJLink] Particle system destroyed");
}

void vjlink_particles_set_config(struct vjlink_particles *ps,
                                  const struct vjlink_emitter_config *config)
{
	if (ps && config)
		ps->config = *config;
}

void vjlink_particles_init_state(struct vjlink_particles *ps)
{
	if (!ps)
		return;

	int size = VJLINK_PARTICLE_TEX_SIZE;

	/* Initialize state texture with random positions */
	float *state_data = malloc(size * size * 4 * sizeof(float));
	float *vel_data = malloc(size * size * 4 * sizeof(float));

	for (int i = 0; i < size * size; i++) {
		/* Random position in a sphere */
		float theta = randf() * 2.0f * (float)M_PI;
		float phi = acosf(2.0f * randf() - 1.0f);
		float r = cbrtf(randf()) * ps->config.radius;

		state_data[i * 4 + 0] = r * sinf(phi) * cosf(theta); /* x */
		state_data[i * 4 + 1] = r * sinf(phi) * sinf(theta); /* y */
		state_data[i * 4 + 2] = r * cosf(phi);               /* z */
		state_data[i * 4 + 3] = randf();                      /* lifetime phase */

		/* Random velocity */
		float speed = ps->config.emit_speed * randf_range(0.5f, 1.5f);
		float vtheta = randf() * 2.0f * (float)M_PI;
		float vphi = randf() * (float)M_PI * ps->config.spread;

		vel_data[i * 4 + 0] = speed * sinf(vphi) * cosf(vtheta);
		vel_data[i * 4 + 1] = speed * cosf(vphi); /* bias upward */
		vel_data[i * 4 + 2] = speed * sinf(vphi) * sinf(vtheta);
		vel_data[i * 4 + 3] = 0.0f; /* age */
	}

	/* Upload to state texture A */
	gs_texrender_reset(ps->state_a);
	if (gs_texrender_begin(ps->state_a, size, size)) {
		/* Create temp texture from data */
		gs_texture_t *temp = gs_texture_create(size, size, GS_RGBA32F, 1,
			(const uint8_t **)&state_data, 0);
		if (temp) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
			gs_effect_set_texture(img, temp);
			gs_ortho(0, (float)size, 0, (float)size, -1, 1);
			while (gs_effect_loop(def, "Draw"))
				gs_draw_sprite(temp, 0, size, size);
			gs_texture_destroy(temp);
		}
		gs_texrender_end(ps->state_a);
	}

	/* Upload velocity texture A */
	gs_texrender_reset(ps->vel_a);
	if (gs_texrender_begin(ps->vel_a, size, size)) {
		gs_texture_t *temp = gs_texture_create(size, size, GS_RGBA32F, 1,
			(const uint8_t **)&vel_data, 0);
		if (temp) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
			gs_effect_set_texture(img, temp);
			gs_ortho(0, (float)size, 0, (float)size, -1, 1);
			while (gs_effect_loop(def, "Draw"))
				gs_draw_sprite(temp, 0, size, size);
			gs_texture_destroy(temp);
		}
		gs_texrender_end(ps->vel_a);
	}

	free(state_data);
	free(vel_data);

	ps->current_is_a = true;
	blog(LOG_INFO, "[VJLink] Particle state initialized");
}

void vjlink_particles_simulate(struct vjlink_particles *ps, float dt)
{
	if (!ps || !ps->sim_effect)
		return;

	struct vjlink_context *ctx = vjlink_get_context();
	int size = VJLINK_PARTICLE_TEX_SIZE;

	/* Get current/next textures */
	gs_texrender_t *state_src = ps->current_is_a ? ps->state_a : ps->state_b;
	gs_texrender_t *state_dst = ps->current_is_a ? ps->state_b : ps->state_a;
	gs_texrender_t *vel_src = ps->current_is_a ? ps->vel_a : ps->vel_b;
	gs_texrender_t *vel_dst = ps->current_is_a ? ps->vel_b : ps->vel_a;

	gs_texture_t *state_tex = gs_texrender_get_texture(state_src);
	gs_texture_t *vel_tex = gs_texrender_get_texture(vel_src);
	gs_texture_t *audio_tex = vjlink_audio_texture_get();

	if (!state_tex || !vel_tex)
		return;

	/* Set simulation uniforms */
	gs_eparam_t *p;

	p = gs_effect_get_param_by_name(ps->sim_effect, "state_tex");
	if (p) gs_effect_set_texture(p, state_tex);

	p = gs_effect_get_param_by_name(ps->sim_effect, "vel_tex");
	if (p) gs_effect_set_texture(p, vel_tex);

	if (audio_tex) {
		p = gs_effect_get_param_by_name(ps->sim_effect, "audio_tex");
		if (p) gs_effect_set_texture(p, audio_tex);
	}

	p = gs_effect_get_param_by_name(ps->sim_effect, "dt");
	if (p) gs_effect_set_float(p, dt);

	p = gs_effect_get_param_by_name(ps->sim_effect, "gravity");
	if (p) gs_effect_set_float(p, ps->config.gravity);

	p = gs_effect_get_param_by_name(ps->sim_effect, "lifetime");
	if (p) gs_effect_set_float(p, ps->config.lifetime);

	p = gs_effect_get_param_by_name(ps->sim_effect, "emit_speed");
	if (p) gs_effect_set_float(p, ps->config.emit_speed);

	struct vec4 bands;
	vec4_set(&bands, ctx->bands[0], ctx->bands[1], ctx->bands[2], ctx->bands[3]);
	p = gs_effect_get_param_by_name(ps->sim_effect, "bands");
	if (p) gs_effect_set_vec4(p, &bands);

	p = gs_effect_get_param_by_name(ps->sim_effect, "audio_force");
	if (p) gs_effect_set_float(p, ps->config.audio_reactive
		? ps->config.audio_force_scale : 0.0f);

	p = gs_effect_get_param_by_name(ps->sim_effect, "time");
	if (p) gs_effect_set_float(p, ctx->elapsed_time);

	/* Render simulation pass to destination state texture */
	gs_texrender_reset(state_dst);
	if (gs_texrender_begin(state_dst, size, size)) {
		gs_ortho(0, (float)size, 0, (float)size, -1, 1);
		while (gs_effect_loop(ps->sim_effect, "SimState"))
			gs_draw_sprite(NULL, 0, size, size);
		gs_texrender_end(state_dst);
	}

	/* Render velocity update to destination velocity texture */
	gs_texrender_reset(vel_dst);
	if (gs_texrender_begin(vel_dst, size, size)) {
		gs_ortho(0, (float)size, 0, (float)size, -1, 1);
		while (gs_effect_loop(ps->sim_effect, "SimVelocity"))
			gs_draw_sprite(NULL, 0, size, size);
		gs_texrender_end(vel_dst);
	}

	/* Swap buffers */
	ps->current_is_a = !ps->current_is_a;
}

gs_texture_t *vjlink_particles_render(struct vjlink_particles *ps)
{
	if (!ps || !ps->render_effect)
		return NULL;

	struct vjlink_context *ctx = vjlink_get_context();

	gs_texrender_t *state_curr = ps->current_is_a ? ps->state_a : ps->state_b;
	gs_texture_t *state_tex = gs_texrender_get_texture(state_curr);
	if (!state_tex)
		return NULL;

	/* Set render uniforms */
	gs_eparam_t *p;

	p = gs_effect_get_param_by_name(ps->render_effect, "state_tex");
	if (p) gs_effect_set_texture(p, state_tex);

	struct vec2 res;
	vec2_set(&res, (float)ps->width, (float)ps->height);
	p = gs_effect_get_param_by_name(ps->render_effect, "resolution");
	if (p) gs_effect_set_vec2(p, &res);

	p = gs_effect_get_param_by_name(ps->render_effect, "particle_size_start");
	if (p) gs_effect_set_float(p, ps->config.size_start);

	p = gs_effect_get_param_by_name(ps->render_effect, "particle_size_end");
	if (p) gs_effect_set_float(p, ps->config.size_end);

	struct vec4 c_start, c_end;
	vec4_set(&c_start, ps->config.color_start[0], ps->config.color_start[1],
	         ps->config.color_start[2], ps->config.color_start[3]);
	vec4_set(&c_end, ps->config.color_end[0], ps->config.color_end[1],
	         ps->config.color_end[2], ps->config.color_end[3]);

	p = gs_effect_get_param_by_name(ps->render_effect, "color_start");
	if (p) gs_effect_set_vec4(p, &c_start);
	p = gs_effect_get_param_by_name(ps->render_effect, "color_end");
	if (p) gs_effect_set_vec4(p, &c_end);

	p = gs_effect_get_param_by_name(ps->render_effect, "tex_size");
	if (p) gs_effect_set_float(p, (float)VJLINK_PARTICLE_TEX_SIZE);

	/* Render to output */
	gs_texrender_reset(ps->output);
	if (gs_texrender_begin(ps->output, ps->width, ps->height)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);

		gs_ortho(0, (float)ps->width, 0, (float)ps->height, -1, 1);

		/* Enable additive blending for particles */
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_ONE);

		while (gs_effect_loop(ps->render_effect, "Render"))
			gs_draw_sprite(NULL, 0, ps->width, ps->height);

		gs_blend_state_pop();
		gs_texrender_end(ps->output);
	}

	return gs_texrender_get_texture(ps->output);
}
