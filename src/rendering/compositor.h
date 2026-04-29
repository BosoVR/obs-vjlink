#pragma once

#include "effect_system.h"
#include "band_effects.h"
#include <obs-module.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* VJLINK_MAX_CHAIN is defined in vjlink_context.h */

/*
 * Compositor - manages the effect chain rendering pipeline.
 *
 * Renders a sequence of effects, each with its own render target.
 * Supports blend modes between layers and feedback (ping-pong buffers).
 */

struct vjlink_compositor {
	/* Effect chain */
	struct vjlink_effect_node chain[VJLINK_MAX_CHAIN];
	uint32_t                  chain_length;

	/* Output dimensions */
	uint32_t width;
	uint32_t height;

	/* Feedback ping-pong buffers */
	gs_texrender_t *feedback_a;
	gs_texrender_t *feedback_b;
	bool            feedback_current; /* false=A is current, true=B */

	/* Per-band effect system */
	struct vjlink_band_effects band_fx;

	/* Blend effect for compositing layers */
	gs_effect_t    *blend_effect;

	/* Seed texture: opaque black at compositor resolution.
	 * Used as initial input_tex before feedback has content. */
	gs_texture_t   *seed_tex;

	/* Cached param handle for feedback copy (avoid per-frame lookup) */
	gs_eparam_t    *cached_default_image;

	/* Transparent background: dark areas become see-through */
	bool transparent_bg;

	/* Luma-to-alpha post-process (lazy-loaded when transparent_bg) */
	gs_effect_t    *luma_alpha_effect;
	gs_eparam_t    *luma_alpha_image;
	gs_texrender_t *luma_alpha_target;

	/* Debug overlay (lazy-loaded when debug_overlay enabled) */
	gs_effect_t    *debug_effect;
	gs_eparam_t    *debug_image;
	gs_eparam_t    *debug_resolution;
	gs_eparam_t    *debug_bands;
	gs_eparam_t    *debug_beat_phase;
	gs_eparam_t    *debug_bpm;
	gs_eparam_t    *debug_time;
	gs_eparam_t    *debug_fps;
	gs_eparam_t    *debug_onset;
	gs_texrender_t *debug_target;
	bool            debug_overlay;
	float           frame_count;
	float           fps_timer;
	float           current_fps;

	/* Initialized flag */
	bool initialized;
};

/* Create / destroy */
struct vjlink_compositor *vjlink_compositor_create_renderer(uint32_t width,
                                                            uint32_t height);
void vjlink_compositor_destroy_renderer(struct vjlink_compositor *comp);

/* Resize output */
void vjlink_compositor_resize(struct vjlink_compositor *comp,
                              uint32_t width, uint32_t height);

/* Set active effect (single effect, clears chain) */
void vjlink_compositor_set_effect(struct vjlink_compositor *comp,
                                  const char *effect_id);

/* Add effect to chain */
bool vjlink_compositor_chain_add(struct vjlink_compositor *comp,
                                 const char *effect_id,
                                 enum vjlink_blend_mode blend_mode,
                                 float blend_alpha);

/* Clear effect chain */
void vjlink_compositor_chain_clear(struct vjlink_compositor *comp);

/* Set parameter on a chain node */
void vjlink_compositor_set_chain_param(struct vjlink_compositor *comp,
                                       uint32_t chain_index,
                                       const char *param_name,
                                       float value);

/*
 * Render one frame of the compositor.
 * Must be called from the OBS graphics thread (inside video_render).
 * Returns the final output texture.
 */
gs_texture_t *vjlink_compositor_render(struct vjlink_compositor *comp,
                                       gs_texture_t *base_tex);

/* Get the previous frame texture (for feedback effects) */
gs_texture_t *vjlink_compositor_get_feedback_tex(struct vjlink_compositor *comp);

#ifdef __cplusplus
}
#endif
