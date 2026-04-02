#pragma once

#include "rendering/effect_system.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VJLINK_MAX_PRESETS 256

/* Preset animation binding */
struct vjlink_anim_binding {
	char  param_name[64];
	char  source_type[16];  /* "lfo", "audio_band", "beat_env", "sequencer" */
	int   band_index;       /* for audio_band: 0-3 */
	int   lfo_index;        /* for lfo: 0-3 */
	float scale;
	float offset;
	float decay_rate;       /* for beat_env */
};

/* Preset definition */
struct vjlink_preset {
	char   id[64];
	char   name[128];
	char   category[32];
	char   description[256];

	/* Effect chain */
	struct {
		char                    effect_id[64];
		bool                    enabled;
		enum vjlink_blend_mode  blend_mode;
		float                   blend_alpha;
		float                   params[VJLINK_MAX_PARAMS][4];
		struct vjlink_anim_binding anims[VJLINK_MAX_PARAMS];
		uint32_t                anim_count;
	} chain[VJLINK_MAX_CHAIN];
	uint32_t chain_length;

	/* LFO config */
	struct {
		int   waveform;     /* 0=sine, 1=tri, 2=saw, 3=square, 4=random */
		float frequency;
		float phase;
		bool  sync_to_beat;
	} lfo_config[VJLINK_NUM_LFOS];
};

/* Initialize preset manager */
bool vjlink_preset_manager_init(void);
void vjlink_preset_manager_shutdown(void);

/* Get preset count */
uint32_t vjlink_preset_get_count(void);

/* Get preset by index or ID */
struct vjlink_preset *vjlink_preset_get(uint32_t index);
struct vjlink_preset *vjlink_preset_find(const char *preset_id);

/* Load preset from JSON file */
bool vjlink_preset_load_file(const char *path);

/* Save current state as preset */
bool vjlink_preset_save_file(const char *path, const struct vjlink_preset *preset);

/* Apply preset to compositor */
bool vjlink_preset_apply(const char *preset_id, struct vjlink_compositor *comp);

/* Scan preset directories for .json files */
void vjlink_preset_scan_directory(const char *path);

/* Get total loaded preset count */
int vjlink_preset_manager_get_count(void);

/* Apply preset by index (for hotkey navigation) */
bool vjlink_preset_apply_by_index(int index);

/* Apply preset by name (for websocket) */
bool vjlink_preset_apply_by_name(const char *name);

/* Set a single parameter on the active effect chain */
void vjlink_preset_set_param(const char *param_name, float value);

#ifdef __cplusplus
}
#endif
