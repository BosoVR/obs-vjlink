#include "vjlink_state.h"
#include "../vjlink_context.h"
#include "presets/cjson/cJSON.h"

#include <obs-module.h>
#include <util/platform.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static struct vjlink_state g_state;
static char g_state_path[1024];
static bool g_initialized = false;

static void copy_string(char *dst, size_t dst_size, const char *src)
{
	if (!dst || dst_size == 0)
		return;

	if (!src)
		src = "";

	strncpy(dst, src, dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static void set_default_state(void)
{
	memset(&g_state, 0, sizeof(g_state));
	g_state.schema_version = VJLINK_STATE_SCHEMA_VERSION;
	copy_string(g_state.ui_mode, sizeof(g_state.ui_mode), "basic");
	copy_string(g_state.ai_mode, sizeof(g_state.ai_mode), "off");
	g_state.strobe_safety = 1.0;
	copy_string(g_state.audio_response_profile,
	            sizeof(g_state.audio_response_profile), "balanced");
	g_state.audio_attack_rate = 0.65;
	g_state.audio_release_rate = 0.22;
	g_state.audio_raw_mix = 0.25;
	g_state.kick_onset_threshold = 0.05;
	g_state.snare_onset_threshold = 0.04;
	g_state.hat_onset_threshold = 0.04;
	g_state.onset_boost = 1.0;
	g_state.peak_decay_rate = 0.995;
}

static double clamp_double(double value, double min_value, double max_value)
{
	if (value < min_value)
		return min_value;
	if (value > max_value)
		return max_value;
	return value;
}

static void apply_audio_response_to_context(void)
{
	struct vjlink_context *ctx = vjlink_get_context();
	if (!ctx)
		return;

	copy_string(ctx->audio_response_profile,
	            sizeof(ctx->audio_response_profile),
	            g_state.audio_response_profile);
	ctx->audio_attack_rate = (float)clamp_double(g_state.audio_attack_rate,
	                                             0.01, 1.0);
	ctx->audio_release_rate = (float)clamp_double(g_state.audio_release_rate,
	                                              0.01, 0.80);
	ctx->audio_fall_rate = ctx->audio_release_rate;
	ctx->audio_raw_mix = (float)clamp_double(g_state.audio_raw_mix,
	                                         0.0, 1.0);
	ctx->kick_onset_threshold = (float)clamp_double(
		g_state.kick_onset_threshold, 0.0, 1.0);
	ctx->snare_onset_threshold = (float)clamp_double(
		g_state.snare_onset_threshold, 0.0, 1.0);
	ctx->hat_onset_threshold = (float)clamp_double(
		g_state.hat_onset_threshold, 0.0, 1.0);
	ctx->onset_boost = (float)clamp_double(g_state.onset_boost,
	                                       0.1, 4.0);
	ctx->peak_decay_rate = (float)clamp_double(g_state.peak_decay_rate,
	                                           0.80, 0.9999);
}

static void build_state_path(void)
{
	if (g_state_path[0])
		return;

	char *anchor = obs_module_file("web-ui/vjlink-control.html");
	if (anchor) {
		char *last_sep = strrchr(anchor, '/');
		if (!last_sep)
			last_sep = strrchr(anchor, '\\');
		if (last_sep)
			*last_sep = '\0';

		last_sep = strrchr(anchor, '/');
		if (!last_sep)
			last_sep = strrchr(anchor, '\\');
		if (last_sep)
			*last_sep = '\0';

		snprintf(g_state_path, sizeof(g_state_path),
		         "%s/state/vjlink_state_v2.json", anchor);
		bfree(anchor);
		return;
	}

	char *path = obs_module_config_path("state/vjlink_state_v2.json");
	if (path) {
		copy_string(g_state_path, sizeof(g_state_path), path);
		bfree(path);
	}
}

static bool ensure_state_dir(void)
{
	char dir[1024];
	char *last_sep;

	if (!g_state_path[0])
		return false;

	copy_string(dir, sizeof(dir), g_state_path);
	last_sep = strrchr(dir, '/');
	if (!last_sep)
		last_sep = strrchr(dir, '\\');
	if (!last_sep)
		return false;

	*last_sep = '\0';
	return os_mkdirs(dir) == MKDIR_SUCCESS || os_file_exists(dir);
}

static bool get_state_base_dir(char *out, size_t out_size)
{
	char *last_sep;

	if (!out || out_size == 0)
		return false;
	if (!g_state_path[0])
		build_state_path();
	if (!g_state_path[0])
		return false;

	copy_string(out, out_size, g_state_path);
	last_sep = strrchr(out, '/');
	if (!last_sep)
		last_sep = strrchr(out, '\\');
	if (!last_sep)
		return false;
	*last_sep = '\0';
	return true;
}

static void safe_profile_name(char *dst, size_t dst_size, const char *name)
{
	size_t out = 0;

	if (!dst || dst_size == 0)
		return;

	if (!name || !*name)
		name = "default";

	for (size_t i = 0; name[i] && out + 1 < dst_size; i++) {
		unsigned char c = (unsigned char)name[i];
		if (isalnum(c) || c == '-' || c == '_') {
			dst[out++] = (char)c;
		} else if (c == ' ' || c == '.') {
			dst[out++] = '_';
		}
	}

	if (out == 0)
		dst[out++] = 'p';
	dst[out] = '\0';
}

static bool build_profile_path(const char *name, char *out, size_t out_size)
{
	char base[1024];
	char safe[128];
	char profile_dir[1024];

	if (!get_state_base_dir(base, sizeof(base)))
		return false;

	snprintf(profile_dir, sizeof(profile_dir), "%s/profiles", base);
	if (os_mkdirs(profile_dir) != MKDIR_SUCCESS &&
	    !os_file_exists(profile_dir)) {
		return false;
	}

	safe_profile_name(safe, sizeof(safe), name);
	snprintf(out, out_size, "%s/%s.json", profile_dir, safe);
	return true;
}

static cJSON *params_to_json(const struct vjlink_saved_param *params,
			     uint32_t count)
{
	cJSON *obj = cJSON_CreateObject();
	if (!obj)
		return NULL;

	for (uint32_t i = 0; i < count && i < VJLINK_STATE_MAX_PARAMS; i++)
		cJSON_AddNumberToObject(obj, params[i].name, params[i].value);

	return obj;
}

static cJSON *state_to_json(void)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;

	cJSON_AddNumberToObject(root, "schema_version", g_state.schema_version);
	cJSON_AddStringToObject(root, "ui_mode", g_state.ui_mode);
	cJSON_AddStringToObject(root, "ai_mode", g_state.ai_mode);
	cJSON_AddNumberToObject(root, "strobe_safety", g_state.strobe_safety);
	cJSON_AddStringToObject(root, "state_path", g_state_path);

	cJSON *audio_response = cJSON_AddObjectToObject(root, "audio_response");
	if (audio_response) {
		cJSON_AddStringToObject(audio_response, "profile",
		                        g_state.audio_response_profile);
		cJSON_AddNumberToObject(audio_response, "attack",
		                         g_state.audio_attack_rate);
		cJSON_AddNumberToObject(audio_response, "release",
		                         g_state.audio_release_rate);
		cJSON_AddNumberToObject(audio_response, "raw_mix",
		                         g_state.audio_raw_mix);
		cJSON_AddNumberToObject(audio_response, "kick_threshold",
		                         g_state.kick_onset_threshold);
		cJSON_AddNumberToObject(audio_response, "snare_threshold",
		                         g_state.snare_onset_threshold);
		cJSON_AddNumberToObject(audio_response, "hat_threshold",
		                         g_state.hat_onset_threshold);
		cJSON_AddNumberToObject(audio_response, "onset_boost",
		                         g_state.onset_boost);
		cJSON_AddNumberToObject(audio_response, "peak_decay",
		                         g_state.peak_decay_rate);
	}

	cJSON *chain = cJSON_AddArrayToObject(root, "chain");
	for (uint32_t i = 0;
	     chain && i < g_state.chain_count && i < VJLINK_STATE_MAX_CHAIN_SLOTS;
	     i++) {
		const struct vjlink_saved_chain_slot *slot = &g_state.chain[i];
		cJSON *node = cJSON_CreateObject();
		if (!node)
			continue;
		cJSON_AddStringToObject(node, "effect_id", slot->effect_id);
		cJSON_AddBoolToObject(node, "enabled", slot->enabled);
		cJSON_AddNumberToObject(node, "blend_mode", slot->blend_mode);
		cJSON_AddNumberToObject(node, "blend_alpha", slot->blend_alpha);
		cJSON *params = params_to_json(slot->params, slot->param_count);
		if (params)
			cJSON_AddItemToObject(node, "params", params);
		cJSON_AddItemToArray(chain, node);
	}

	cJSON *audio = cJSON_AddArrayToObject(root, "audio_mappings");
	for (uint32_t i = 0; audio && i < g_state.audio_mapping_count &&
	     i < VJLINK_STATE_MAX_AUDIO_MAPPINGS; i++) {
		const struct vjlink_saved_audio_mapping *mapping =
			&g_state.audio_mappings[i];
		cJSON *node = cJSON_CreateObject();
		if (!node)
			continue;
		cJSON_AddStringToObject(node, "target", mapping->target);
		cJSON_AddNumberToObject(node, "band", mapping->band);
		cJSON_AddNumberToObject(node, "amount", mapping->amount);
		cJSON_AddItemToArray(audio, node);
	}

	cJSON *bands = cJSON_AddArrayToObject(root, "band_slots");
	for (uint32_t i = 0; bands && i < g_state.band_slot_count &&
	     i < VJLINK_STATE_MAX_BAND_SLOTS; i++) {
		const struct vjlink_saved_band_slot *slot = &g_state.band_slots[i];
		cJSON *node = cJSON_CreateObject();
		if (!node)
			continue;
		cJSON_AddNumberToObject(node, "band", slot->band);
		cJSON_AddStringToObject(node, "effect_id", slot->effect_id);
		cJSON_AddBoolToObject(node, "enabled", slot->enabled);
		cJSON_AddNumberToObject(node, "threshold", slot->threshold);
		cJSON_AddNumberToObject(node, "intensity", slot->intensity);
		cJSON_AddNumberToObject(node, "attack", slot->attack);
		cJSON_AddNumberToObject(node, "release", slot->release);
		cJSON_AddNumberToObject(node, "hold", slot->hold);
		cJSON_AddItemToArray(bands, node);
	}

	cJSON *pads = cJSON_AddArrayToObject(root, "pads");
	for (uint32_t i = 0; pads && i < g_state.pad_count &&
	     i < VJLINK_STATE_MAX_PADS; i++) {
		const struct vjlink_saved_pad *pad = &g_state.pads[i];
		cJSON *node = cJSON_CreateObject();
		if (!node)
			continue;
		cJSON_AddNumberToObject(node, "index", pad->index);
		cJSON_AddStringToObject(node, "label", pad->label);
		cJSON_AddStringToObject(node, "action", pad->action);
		cJSON_AddStringToObject(node, "target", pad->target);
		cJSON_AddNumberToObject(node, "value", pad->value);
		cJSON_AddItemToArray(pads, node);
	}

	cJSON *profiles = cJSON_AddArrayToObject(root, "profiles");
	for (uint32_t i = 0; profiles && i < g_state.profile_count &&
	     i < VJLINK_STATE_MAX_PROFILES; i++) {
		const struct vjlink_saved_profile *profile = &g_state.profiles[i];
		cJSON *node = cJSON_CreateObject();
		if (!node)
			continue;
		cJSON_AddStringToObject(node, "id", profile->id);
		cJSON_AddStringToObject(node, "name", profile->name);
		cJSON_AddItemToArray(profiles, node);
	}

	return root;
}

static void parse_params(cJSON *params, struct vjlink_saved_chain_slot *slot)
{
	uint32_t count = 0;
	cJSON *param;

	if (!cJSON_IsObject(params))
		return;

	for (param = params->child; param && count < VJLINK_STATE_MAX_PARAMS;
	     param = param->next) {
		if (!cJSON_IsNumber(param) || !param->string)
			continue;

		copy_string(slot->params[count].name,
			    sizeof(slot->params[count].name), param->string);
		slot->params[count].value = param->valuedouble;
		count++;
	}
	slot->param_count = count;
}

static void parse_state(cJSON *root)
{
	cJSON *item;

	set_default_state();

	item = cJSON_GetObjectItem(root, "ui_mode");
	if (cJSON_IsString(item))
		copy_string(g_state.ui_mode, sizeof(g_state.ui_mode),
			    item->valuestring);

	item = cJSON_GetObjectItem(root, "ai_mode");
	if (cJSON_IsString(item))
		copy_string(g_state.ai_mode, sizeof(g_state.ai_mode),
			    item->valuestring);

	item = cJSON_GetObjectItem(root, "strobe_safety");
	if (cJSON_IsNumber(item))
		g_state.strobe_safety = item->valuedouble;

	cJSON *audio_response = cJSON_GetObjectItem(root, "audio_response");
	if (cJSON_IsObject(audio_response)) {
		item = cJSON_GetObjectItem(audio_response, "profile");
		if (cJSON_IsString(item))
			copy_string(g_state.audio_response_profile,
			            sizeof(g_state.audio_response_profile),
			            item->valuestring);
		item = cJSON_GetObjectItem(audio_response, "attack");
		if (cJSON_IsNumber(item))
			g_state.audio_attack_rate = item->valuedouble;
		item = cJSON_GetObjectItem(audio_response, "release");
		if (cJSON_IsNumber(item))
			g_state.audio_release_rate = item->valuedouble;
		item = cJSON_GetObjectItem(audio_response, "raw_mix");
		if (cJSON_IsNumber(item))
			g_state.audio_raw_mix = item->valuedouble;
		item = cJSON_GetObjectItem(audio_response, "kick_threshold");
		if (cJSON_IsNumber(item))
			g_state.kick_onset_threshold = item->valuedouble;
		item = cJSON_GetObjectItem(audio_response, "snare_threshold");
		if (cJSON_IsNumber(item))
			g_state.snare_onset_threshold = item->valuedouble;
		item = cJSON_GetObjectItem(audio_response, "hat_threshold");
		if (cJSON_IsNumber(item))
			g_state.hat_onset_threshold = item->valuedouble;
		item = cJSON_GetObjectItem(audio_response, "onset_boost");
		if (cJSON_IsNumber(item))
			g_state.onset_boost = item->valuedouble;
		item = cJSON_GetObjectItem(audio_response, "peak_decay");
		if (cJSON_IsNumber(item))
			g_state.peak_decay_rate = item->valuedouble;
	}

	cJSON *chain = cJSON_GetObjectItem(root, "chain");
	if (cJSON_IsArray(chain)) {
		uint32_t count = 0;
		cJSON *node;
		cJSON_ArrayForEach(node, chain) {
			if (!cJSON_IsObject(node) ||
			    count >= VJLINK_STATE_MAX_CHAIN_SLOTS)
				continue;

			struct vjlink_saved_chain_slot *slot =
				&g_state.chain[count];
			item = cJSON_GetObjectItem(node, "effect_id");
			if (cJSON_IsString(item))
				copy_string(slot->effect_id,
					    sizeof(slot->effect_id),
					    item->valuestring);
			item = cJSON_GetObjectItem(node, "enabled");
			slot->enabled = !item || cJSON_IsTrue(item);
			item = cJSON_GetObjectItem(node, "blend_mode");
			slot->blend_mode = cJSON_IsNumber(item) ?
				item->valueint : 0;
			item = cJSON_GetObjectItem(node, "blend_alpha");
			slot->blend_alpha = cJSON_IsNumber(item) ?
				item->valuedouble : 1.0;
			parse_params(cJSON_GetObjectItem(node, "params"), slot);
			count++;
		}
		g_state.chain_count = count;
	}

	cJSON *audio = cJSON_GetObjectItem(root, "audio_mappings");
	if (cJSON_IsArray(audio)) {
		uint32_t count = 0;
		cJSON *node;
		cJSON_ArrayForEach(node, audio) {
			if (!cJSON_IsObject(node) ||
			    count >= VJLINK_STATE_MAX_AUDIO_MAPPINGS)
				continue;

			struct vjlink_saved_audio_mapping *mapping =
				&g_state.audio_mappings[count];
			item = cJSON_GetObjectItem(node, "target");
			if (cJSON_IsString(item))
				copy_string(mapping->target,
					    sizeof(mapping->target),
					    item->valuestring);
			item = cJSON_GetObjectItem(node, "band");
			mapping->band = cJSON_IsNumber(item) ? item->valueint : 0;
			item = cJSON_GetObjectItem(node, "amount");
			mapping->amount = cJSON_IsNumber(item) ?
				item->valuedouble : 0.0;
			count++;
		}
		g_state.audio_mapping_count = count;
	}

	cJSON *bands = cJSON_GetObjectItem(root, "band_slots");
	if (cJSON_IsArray(bands)) {
		uint32_t count = 0;
		cJSON *node;
		cJSON_ArrayForEach(node, bands) {
			if (!cJSON_IsObject(node) ||
			    count >= VJLINK_STATE_MAX_BAND_SLOTS)
				continue;

			struct vjlink_saved_band_slot *slot =
				&g_state.band_slots[count];
			item = cJSON_GetObjectItem(node, "band");
			slot->band = cJSON_IsNumber(item) ? item->valueint : 0;
			item = cJSON_GetObjectItem(node, "effect_id");
			if (cJSON_IsString(item))
				copy_string(slot->effect_id,
					    sizeof(slot->effect_id),
					    item->valuestring);
			item = cJSON_GetObjectItem(node, "enabled");
			slot->enabled = !item || cJSON_IsTrue(item);
			item = cJSON_GetObjectItem(node, "threshold");
			slot->threshold = cJSON_IsNumber(item) ?
				item->valuedouble : 0.0;
			item = cJSON_GetObjectItem(node, "intensity");
			slot->intensity = cJSON_IsNumber(item) ?
				item->valuedouble : 0.0;
			item = cJSON_GetObjectItem(node, "attack");
			slot->attack = cJSON_IsNumber(item) ?
				item->valuedouble : 0.65;
			item = cJSON_GetObjectItem(node, "release");
			slot->release = cJSON_IsNumber(item) ?
				item->valuedouble : 0.18;
			item = cJSON_GetObjectItem(node, "hold");
			slot->hold = cJSON_IsNumber(item) ?
				item->valuedouble : 6.0;
			count++;
		}
		g_state.band_slot_count = count;
	}

	cJSON *pads = cJSON_GetObjectItem(root, "pads");
	if (cJSON_IsArray(pads)) {
		uint32_t count = 0;
		cJSON *node;
		cJSON_ArrayForEach(node, pads) {
			if (!cJSON_IsObject(node) ||
			    count >= VJLINK_STATE_MAX_PADS)
				continue;

			struct vjlink_saved_pad *pad = &g_state.pads[count];
			item = cJSON_GetObjectItem(node, "index");
			pad->index = cJSON_IsNumber(item) ? item->valueint : 0;
			item = cJSON_GetObjectItem(node, "label");
			if (cJSON_IsString(item))
				copy_string(pad->label, sizeof(pad->label),
					    item->valuestring);
			item = cJSON_GetObjectItem(node, "action");
			if (cJSON_IsString(item))
				copy_string(pad->action, sizeof(pad->action),
					    item->valuestring);
			item = cJSON_GetObjectItem(node, "target");
			if (cJSON_IsString(item))
				copy_string(pad->target, sizeof(pad->target),
					    item->valuestring);
			item = cJSON_GetObjectItem(node, "value");
			pad->value = cJSON_IsNumber(item) ?
				item->valuedouble : 0.0;
			count++;
		}
		g_state.pad_count = count;
	}

	cJSON *profiles = cJSON_GetObjectItem(root, "profiles");
	if (cJSON_IsArray(profiles)) {
		uint32_t count = 0;
		cJSON *node;
		cJSON_ArrayForEach(node, profiles) {
			if (!cJSON_IsObject(node) ||
			    count >= VJLINK_STATE_MAX_PROFILES)
				continue;

			struct vjlink_saved_profile *profile =
				&g_state.profiles[count];
			item = cJSON_GetObjectItem(node, "id");
			if (cJSON_IsString(item))
				copy_string(profile->id, sizeof(profile->id),
					    item->valuestring);
			item = cJSON_GetObjectItem(node, "name");
			if (cJSON_IsString(item))
				copy_string(profile->name, sizeof(profile->name),
					    item->valuestring);
			count++;
		}
		g_state.profile_count = count;
	}

	apply_audio_response_to_context();
}

bool vjlink_state_init(void)
{
	build_state_path();
	set_default_state();
	g_initialized = g_state_path[0] != '\0';

	if (!g_initialized)
		blog(LOG_WARNING, "[VJLink] State path could not be resolved");

	return g_initialized;
}

void vjlink_state_shutdown(void)
{
	memset(&g_state, 0, sizeof(g_state));
	g_initialized = false;
}

bool vjlink_state_load_default(void)
{
	if (!g_initialized && !vjlink_state_init())
		return false;

	char *json = os_quick_read_utf8_file(g_state_path);
	if (!json) {
		set_default_state();
		return vjlink_state_save_default();
	}

	bool ok = vjlink_state_apply_json(json);
	bfree(json);
	return ok;
}

bool vjlink_state_save_default(void)
{
	char *json = NULL;
	bool ok = false;

	if (!g_initialized && !vjlink_state_init())
		return false;

	if (!ensure_state_dir())
		return false;

	if (!vjlink_state_export_json(&json) || !json)
		return false;

	FILE *file = fopen(g_state_path, "wb");
	if (file) {
		size_t len = strlen(json);
		ok = fwrite(json, 1, len, file) == len;
		ok = fclose(file) == 0 && ok;
	}

	bfree(json);
	return ok;
}

bool vjlink_state_export_json(char **out_json)
{
	cJSON *root;
	char *printed;

	if (!out_json)
		return false;
	*out_json = NULL;

	root = state_to_json();
	if (!root)
		return false;

	printed = cJSON_Print(root);
	cJSON_Delete(root);
	if (!printed)
		return false;

	*out_json = bstrdup(printed);
	cJSON_free(printed);
	return *out_json != NULL;
}

bool vjlink_state_apply_json(const char *json)
{
	cJSON *root;
	cJSON *schema;

	if (!json || !*json)
		return false;

	root = cJSON_Parse(json);
	if (!root)
		return false;

	schema = cJSON_GetObjectItem(root, "schema_version");
	if (!cJSON_IsNumber(schema) ||
	    schema->valueint != VJLINK_STATE_SCHEMA_VERSION) {
		cJSON_Delete(root);
		return false;
	}

	parse_state(root);
	cJSON_Delete(root);
	return true;
}

const char *vjlink_state_get_path(void)
{
	if (!g_state_path[0])
		build_state_path();
	return g_state_path;
}

bool vjlink_state_save_profile(const char *name, const char *json)
{
	char path[1024];
	FILE *file;
	size_t len;
	bool ok;

	if (!json || !*json)
		return false;
	if (!build_profile_path(name, path, sizeof(path)))
		return false;

	file = fopen(path, "wb");
	if (!file)
		return false;

	len = strlen(json);
	ok = fwrite(json, 1, len, file) == len;
	ok = fclose(file) == 0 && ok;
	return ok;
}

bool vjlink_state_load_profile(const char *name, char **out_json)
{
	char path[1024];
	char *json;

	if (!out_json)
		return false;
	*out_json = NULL;
	if (!build_profile_path(name, path, sizeof(path)))
		return false;

	json = os_quick_read_utf8_file(path);
	if (!json)
		return false;

	*out_json = bstrdup(json);
	bfree(json);
	return *out_json != NULL;
}

bool vjlink_state_delete_profile(const char *name)
{
	char path[1024];

	if (!build_profile_path(name, path, sizeof(path)))
		return false;
	return os_unlink(path) == 0;
}

bool vjlink_state_list_profiles(char ***out_names, uint32_t *out_count)
{
	char base[1024];
	char profile_dir[1024];
	os_dir_t *dir;
	struct os_dirent *ent;
	char **names = NULL;
	uint32_t count = 0;
	uint32_t capacity = 0;

	if (!out_names || !out_count)
		return false;
	*out_names = NULL;
	*out_count = 0;

	if (!get_state_base_dir(base, sizeof(base)))
		return false;
	snprintf(profile_dir, sizeof(profile_dir), "%s/profiles", base);

	dir = os_opendir(profile_dir);
	if (!dir)
		return true;

	while ((ent = os_readdir(dir)) != NULL) {
		const char *ext;
		size_t len;
		char clean[128];

		if (ent->directory)
			continue;
		ext = strrchr(ent->d_name, '.');
		if (!ext || strcmp(ext, ".json") != 0)
			continue;

		if (count >= capacity) {
			uint32_t new_capacity = capacity ? capacity * 2 : 8;
			char **new_names = brealloc(names,
			                            sizeof(char *) * new_capacity);
			if (!new_names)
				break;
			names = new_names;
			capacity = new_capacity;
		}

		len = (size_t)(ext - ent->d_name);
		if (len >= sizeof(clean))
			len = sizeof(clean) - 1;
		memcpy(clean, ent->d_name, len);
		clean[len] = '\0';
		names[count] = bstrdup(clean);
		if (names[count])
			count++;
	}

	os_closedir(dir);
	*out_names = names;
	*out_count = count;
	return true;
}

void vjlink_state_free_profile_list(char **names, uint32_t count)
{
	if (!names)
		return;
	for (uint32_t i = 0; i < count; i++)
		bfree(names[i]);
	bfree(names);
}

void vjlink_state_set_audio_response(const char *profile, double attack,
                                     double release, double raw_mix,
                                     double kick_threshold,
                                     double snare_threshold,
                                     double hat_threshold,
                                     double onset_boost,
                                     double peak_decay)
{
	if (!g_initialized)
		vjlink_state_init();

	copy_string(g_state.audio_response_profile,
	            sizeof(g_state.audio_response_profile),
	            (profile && *profile) ? profile : "manual");
	g_state.audio_attack_rate = clamp_double(attack, 0.01, 1.0);
	g_state.audio_release_rate = clamp_double(release, 0.01, 0.80);
	g_state.audio_raw_mix = clamp_double(raw_mix, 0.0, 1.0);
	g_state.kick_onset_threshold = clamp_double(kick_threshold, 0.0, 1.0);
	g_state.snare_onset_threshold = clamp_double(snare_threshold, 0.0, 1.0);
	g_state.hat_onset_threshold = clamp_double(hat_threshold, 0.0, 1.0);
	g_state.onset_boost = clamp_double(onset_boost, 0.1, 4.0);
	g_state.peak_decay_rate = clamp_double(peak_decay, 0.80, 0.9999);
	apply_audio_response_to_context();
}
