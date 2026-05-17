#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VJLINK_STATE_SCHEMA_VERSION 2
#define VJLINK_STATE_MAX_PARAMS 32
#define VJLINK_STATE_MAX_CHAIN_SLOTS 8
#define VJLINK_STATE_MAX_AUDIO_MAPPINGS 32
#define VJLINK_STATE_MAX_BAND_SLOTS 4
#define VJLINK_STATE_MAX_PADS 128
#define VJLINK_STATE_MAX_PROFILES 16

struct vjlink_saved_param {
	char name[64];
	double value;
};

struct vjlink_saved_chain_slot {
	char effect_id[64];
	bool enabled;
	int blend_mode;
	double blend_alpha;
	struct vjlink_saved_param params[VJLINK_STATE_MAX_PARAMS];
	uint32_t param_count;
};

struct vjlink_saved_audio_mapping {
	char target[64];
	int band;
	double amount;
};

struct vjlink_saved_band_slot {
	int band;
	char effect_id[64];
	bool enabled;
	double threshold;
	double intensity;
	double attack;
	double release;
	double hold;
};

struct vjlink_saved_pad {
	int index;
	char label[64];
	char action[64];
	char target[64];
	double value;
};

struct vjlink_saved_profile {
	char id[64];
	char name[64];
};

struct vjlink_state {
	int schema_version;
	char ui_mode[16];
	char ai_mode[16];
	double strobe_safety;

	struct vjlink_saved_chain_slot chain[VJLINK_STATE_MAX_CHAIN_SLOTS];
	uint32_t chain_count;
	struct vjlink_saved_audio_mapping audio_mappings[VJLINK_STATE_MAX_AUDIO_MAPPINGS];
	uint32_t audio_mapping_count;
	struct vjlink_saved_band_slot band_slots[VJLINK_STATE_MAX_BAND_SLOTS];
	uint32_t band_slot_count;
	struct vjlink_saved_pad pads[VJLINK_STATE_MAX_PADS];
	uint32_t pad_count;
	struct vjlink_saved_profile profiles[VJLINK_STATE_MAX_PROFILES];
	uint32_t profile_count;
};

bool vjlink_state_init(void);
void vjlink_state_shutdown(void);
bool vjlink_state_load_default(void);
bool vjlink_state_save_default(void);
bool vjlink_state_export_json(char **out_json);
bool vjlink_state_apply_json(const char *json);
const char *vjlink_state_get_path(void);
bool vjlink_state_save_profile(const char *name, const char *json);
bool vjlink_state_load_profile(const char *name, char **out_json);
bool vjlink_state_delete_profile(const char *name);
bool vjlink_state_list_profiles(char ***out_names, uint32_t *out_count);
void vjlink_state_free_profile_list(char **names, uint32_t count);

#ifdef __cplusplus
}
#endif
