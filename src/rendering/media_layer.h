#pragma once

#include "effect_system.h"
#include "vjlink_context.h"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VJLINK_MAX_MEDIA_LAYERS 4

/* How the media layer is triggered by audio */
enum vjlink_media_trigger_mode {
	VJLINK_MEDIA_ALWAYS_ON = 0,
	VJLINK_MEDIA_BAND_TRIGGER,
	VJLINK_MEDIA_BEAT_TRIGGER,
	VJLINK_MEDIA_GATE,
};

/* A single media layer (image/GIF overlay) */
struct vjlink_media_layer {
	gs_image_file_t  image;
	char             path[512];
	bool             loaded;
	bool             enabled;

	/* Audio trigger settings */
	enum vjlink_media_trigger_mode trigger_mode;
	int              trigger_band;    /* 0-3 */
	float            threshold;
	float            intensity;       /* opacity multiplier */

	/* Display settings */
	float            opacity;         /* base opacity 0-1 */
	enum vjlink_blend_mode blend_mode;
	float            scale;           /* 0.1-4.0 */
	float            pos_x, pos_y;    /* normalized 0-1 */

	/* Runtime */
	float            current_opacity; /* computed per frame */
};

/* Media layer system */
struct vjlink_media_layers {
	struct vjlink_media_layer layers[VJLINK_MAX_MEDIA_LAYERS];
	uint32_t width, height;
	bool     initialized;
};

/* Initialize media layer system */
void vjlink_media_layers_init(struct vjlink_media_layers *ml,
                               uint32_t width, uint32_t height);

/* Destroy all media layer resources */
void vjlink_media_layers_destroy(struct vjlink_media_layers *ml);

/* Set a media layer (loads image/GIF from path) */
void vjlink_media_layer_set(struct vjlink_media_layers *ml, int index,
                             const char *path,
                             enum vjlink_media_trigger_mode trigger_mode,
                             int trigger_band, float threshold,
                             float intensity);

/* Clear a media layer */
void vjlink_media_layer_clear(struct vjlink_media_layers *ml, int index);

/* Update and render all media layers (call per frame from graphics thread) */
void vjlink_media_layers_render(struct vjlink_media_layers *ml,
                                 uint32_t canvas_w, uint32_t canvas_h);

#ifdef __cplusplus
}
#endif
