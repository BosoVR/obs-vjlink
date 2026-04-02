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
	enum vjlink_blend_mode     blend_mode;
	float                      blend_alpha;

	/* Custom parameter values for this band's effect */
	float                      param_values[VJLINK_MAX_PARAMS][4];
};

/* Band effects system: 4 slots, one per audio band */
struct vjlink_band_effects {
	struct vjlink_band_slot slots[VJLINK_NUM_BANDS];
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

/* Clear a band slot */
void vjlink_band_effects_clear_slot(struct vjlink_band_effects *bfx, int band);

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
