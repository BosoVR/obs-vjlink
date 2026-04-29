#pragma once

#include "effect_system.h"
#include "vjlink_context.h"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-band effect slot: one effect assigned to one audio band */
struct vjlink_band_slot {
	char                       effect_id[64];
	struct vjlink_effect_entry *entry;
	gs_texrender_t            *render_target;
	bool                       enabled;
	float                      threshold;        /* 0.0-1.0 */
	float                      intensity;        /* 0.0-2.0 */
	float                      current_activation; /* computed per frame */
	float                      target_activation;
	float                      attack;           /* 0.01-1.0, higher = faster */
	float                      release;          /* 0.01-1.0, higher = faster */
	float                      hold_frames;      /* frames to hold after trigger */
	float                      hold_remaining;
	enum vjlink_blend_mode     blend_mode;
	float                      blend_alpha;

	/* Custom parameter values for this band's effect */
	float                      param_values[VJLINK_MAX_PARAMS][4];
};

/* Band effects system: 4 slots, one per audio band */
struct vjlink_band_effects {
	struct vjlink_band_slot slots[VJLINK_NUM_BANDS];
	/* Render order: layer index 0 = bottom, last = top.
	 * Default: [0, 1, 2, 3] = Bass at bottom, Treble on top.
	 * Values are band indices (0..3); each must appear exactly once. */
	int      render_order[VJLINK_NUM_BANDS];
	gs_texrender_t *composite_target;  /* ping-pong buffer A */
	gs_texrender_t *composite_target2; /* ping-pong buffer B */
	uint32_t width;
	uint32_t height;
	int      log_counter;
	bool     initialized;
};

/* Initialize band effects (call from graphics thread) */
void vjlink_band_effects_init(struct vjlink_band_effects *bfx,
                               uint32_t width, uint32_t height);

/* Destroy band effects resources */
void vjlink_band_effects_destroy(struct vjlink_band_effects *bfx);

/* Resize render targets */
void vjlink_band_effects_resize(struct vjlink_band_effects *bfx,
                                 uint32_t width, uint32_t height);

/* Set effect for a specific band (0=bass, 1=lowmid, 2=highmid, 3=treble) */
void vjlink_band_effects_set_slot(struct vjlink_band_effects *bfx,
                                   int band, const char *effect_id,
                                   float threshold, float intensity);

void vjlink_band_effects_set_slot_response(struct vjlink_band_effects *bfx,
                                           int band, float attack,
                                           float release, float hold_frames);

/* Clear a band slot */
void vjlink_band_effects_clear_slot(struct vjlink_band_effects *bfx, int band);

/* Set render order: order[i] = band index that should render at layer i.
 * Layer 0 is rendered first (bottom), last is on top. */
void vjlink_band_effects_set_order(struct vjlink_band_effects *bfx,
                                    const int order[VJLINK_NUM_BANDS]);

/* Update activation values based on current audio (call per frame) */
void vjlink_band_effects_update(struct vjlink_band_effects *bfx);

/*
 * Render all active band effects and composite onto the scene.
 * input_tex: the current compositor output (scene so far)
 * Returns the composited output texture.
 */
gs_texture_t *vjlink_band_effects_render(struct vjlink_band_effects *bfx,
                                          gs_texture_t *input_tex);

#ifdef __cplusplus
}
#endif
