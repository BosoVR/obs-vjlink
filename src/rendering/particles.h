#pragma once

#include "vjlink_context.h"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Particle texture dimensions: 128x128 = 16384 particles */
#define VJLINK_PARTICLE_TEX_SIZE  128
#define VJLINK_PARTICLE_COUNT     (VJLINK_PARTICLE_TEX_SIZE * VJLINK_PARTICLE_TEX_SIZE)

/* Emitter shapes */
enum vjlink_emitter_shape {
	VJLINK_EMIT_POINT = 0,
	VJLINK_EMIT_SPHERE,
	VJLINK_EMIT_RING,
	VJLINK_EMIT_PLANE,
};

/* Particle emitter configuration */
struct vjlink_emitter_config {
	enum vjlink_emitter_shape shape;
	float position[3];
	float radius;
	float emit_speed;        /* initial velocity magnitude */
	float spread;            /* velocity cone spread (0=focused, 1=hemisphere) */
	float lifetime;          /* particle lifetime in seconds */
	float gravity;           /* downward force */
	float size_start;        /* particle start size */
	float size_end;          /* particle end size */
	float color_start[4];    /* RGBA start */
	float color_end[4];      /* RGBA end */
	bool  audio_reactive;    /* bass triggers velocity burst */
	float audio_force_scale; /* scale for audio force */
};

/* Particle system state */
struct vjlink_particles {
	/* State textures (ping-pong): RGBA32F
	 * R=pos.x, G=pos.y, B=pos.z, A=lifetime (0..1) */
	gs_texrender_t *state_a;
	gs_texrender_t *state_b;
	bool current_is_a;

	/* Velocity textures (ping-pong): RGBA32F
	 * R=vel.x, G=vel.y, B=vel.z, A=age */
	gs_texrender_t *vel_a;
	gs_texrender_t *vel_b;

	/* Simulation shader */
	gs_effect_t *sim_effect;

	/* Render shader */
	gs_effect_t *render_effect;

	/* Configuration */
	struct vjlink_emitter_config config;

	/* Output render target */
	gs_texrender_t *output;

	uint32_t width;
	uint32_t height;

	bool initialized;
	float time_accumulator;
};

/* Create / destroy */
struct vjlink_particles *vjlink_particles_create(uint32_t render_width,
                                                  uint32_t render_height);
void vjlink_particles_destroy(struct vjlink_particles *ps);

/* Set emitter configuration */
void vjlink_particles_set_config(struct vjlink_particles *ps,
                                  const struct vjlink_emitter_config *config);

/* Initialize particle state (randomize positions, call from graphics thread) */
void vjlink_particles_init_state(struct vjlink_particles *ps);

/* Simulate one step (call from graphics thread) */
void vjlink_particles_simulate(struct vjlink_particles *ps, float dt);

/* Render particles to output texture (call from graphics thread) */
gs_texture_t *vjlink_particles_render(struct vjlink_particles *ps);

#ifdef __cplusplus
}
#endif
