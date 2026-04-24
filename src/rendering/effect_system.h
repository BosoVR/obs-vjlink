#pragma once

#include "vjlink_context.h"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/effect.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of custom parameters per effect */
#define VJLINK_MAX_PARAMS 32

/* Maximum registered effects */
#define VJLINK_MAX_REGISTERED_EFFECTS 128

/* Blend modes for effect chain layers */
enum vjlink_blend_mode {
	VJLINK_BLEND_NORMAL = 0,
	VJLINK_BLEND_ADD,
	VJLINK_BLEND_MULTIPLY,
	VJLINK_BLEND_SCREEN,
};

/* Parameter types */
enum vjlink_param_type {
	VJLINK_PARAM_FLOAT = 0,
	VJLINK_PARAM_INT,
	VJLINK_PARAM_BOOL,
	VJLINK_PARAM_COLOR,  /* float4 RGBA */
	VJLINK_PARAM_VEC2,
	VJLINK_PARAM_VEC4,
};

/* Custom parameter definition (from JSON metadata) */
struct vjlink_param_def {
	char                   name[64];
	char                   label[128];
	enum vjlink_param_type type;
	float                  default_val[4]; /* up to vec4 */
	float                  min_val;
	float                  max_val;
	float                  step;
};

/* Loaded effect entry */
struct vjlink_effect_entry {
	char             id[64];          /* e.g. "plasma_classic" */
	char             name[128];       /* display name */
	char             category[32];    /* e.g. "plasma" */
	char             effect_path[512];
	gs_effect_t     *effect;          /* compiled OBS effect */
	bool             loaded;
	bool             hidden;          /* true = not shown in UI dropdowns */
	time_t           file_mtime;      /* last modification time for hot-reload */

	/* Standard uniform handles (cached) */
	gs_eparam_t     *p_view_proj;
	gs_eparam_t     *p_audio_tex;
	gs_eparam_t     *p_image;       /* "image" uniform (filter effects) */
	gs_eparam_t     *p_input_tex;   /* "input_tex" uniform (alt name) */
	gs_eparam_t     *p_prev_tex;
	gs_eparam_t     *p_resolution;
	gs_eparam_t     *p_time;
	gs_eparam_t     *p_bands;
	gs_eparam_t     *p_beat_phase;
	gs_eparam_t     *p_bpm;
	gs_eparam_t     *p_quality;
	gs_eparam_t     *p_band_activation;
	gs_eparam_t     *p_has_input;     /* "has_input_source" uniform */
	gs_eparam_t     *p_logo_tex;     /* "logo_tex" uniform */
	gs_eparam_t     *p_logo_tex2;    /* "logo_tex2" uniform */
	gs_eparam_t     *p_logo_tex3;    /* "logo_tex3" uniform */

	/* Custom parameters */
	struct vjlink_param_def  params[VJLINK_MAX_PARAMS];
	gs_eparam_t             *param_handles[VJLINK_MAX_PARAMS];
	uint32_t                 param_count;
};

/* Effect chain node (runtime state) */
struct vjlink_effect_node {
	char                    effect_id[64];
	struct vjlink_effect_entry *entry;  /* pointer into registry */
	gs_texrender_t         *output;     /* this node's render target */
	bool                    enabled;
	float                   blend_alpha;
	enum vjlink_blend_mode  blend_mode;

	/* Current parameter values */
	float                   param_values[VJLINK_MAX_PARAMS][4];
};

/* Effect system interface */

/* Initialize the effect system: scan effects directory, load metadata */
bool vjlink_effect_system_init(void);

/* Shutdown and free all resources */
void vjlink_effect_system_shutdown(void);

/* Get effect registry count */
uint32_t vjlink_effect_system_get_count(void);

/* Get effect entry by index */
struct vjlink_effect_entry *vjlink_effect_system_get_entry(uint32_t index);

/* Get effect entry by ID */
struct vjlink_effect_entry *vjlink_effect_system_find(const char *effect_id);

/* Ensure an effect is compiled (call from graphics thread) */
bool vjlink_effect_ensure_loaded(struct vjlink_effect_entry *entry);

/*
 * Bind standard uniforms on an effect before drawing.
 * input_tex: the source/previous texture (can be NULL)
 * prev_tex: previous frame for feedback (can be NULL)
 * width, height: output dimensions
 */
void vjlink_effect_bind_uniforms(struct vjlink_effect_entry *entry,
                                 gs_texture_t *input_tex,
                                 gs_texture_t *prev_tex,
                                 uint32_t width, uint32_t height);

/* Bind all custom parameter values from a float[][4] array.
 * If values is NULL, uses the default values from metadata. */
void vjlink_effect_bind_custom_params(struct vjlink_effect_entry *entry,
                                       const float values[][4]);

/* Bind a custom parameter value */
void vjlink_effect_set_param_float(struct vjlink_effect_entry *entry,
                                   const char *name, float value);
void vjlink_effect_set_param_vec4(struct vjlink_effect_entry *entry,
                                  const char *name, const struct vec4 *value);

/* Hot-reload: check if effect file was modified, recompile if so.
 * Returns true if effect was reloaded. Call from graphics thread. */
bool vjlink_effect_check_hot_reload(struct vjlink_effect_entry *entry);

#ifdef __cplusplus
}
#endif
