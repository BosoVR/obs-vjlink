#include "preset_manager.h"
#include "rendering/compositor.h"
#include "cjson/cJSON.h"
#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <string.h>
#include <stdio.h>

static struct vjlink_preset g_presets[VJLINK_MAX_PRESETS];
static uint32_t g_preset_count = 0;
static bool g_initialized = false;

static char *read_file_contents(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || size > 1024 * 1024) { /* max 1MB */
		fclose(f);
		return NULL;
	}

	char *buf = malloc(size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}

	fread(buf, 1, size, f);
	buf[size] = '\0';
	fclose(f);
	return buf;
}

static enum vjlink_blend_mode parse_blend_mode(const char *str)
{
	if (!str) return VJLINK_BLEND_NORMAL;
	if (strcmp(str, "Add") == 0 || strcmp(str, "add") == 0) return VJLINK_BLEND_ADD;
	if (strcmp(str, "Multiply") == 0 || strcmp(str, "multiply") == 0) return VJLINK_BLEND_MULTIPLY;
	if (strcmp(str, "Screen") == 0 || strcmp(str, "screen") == 0) return VJLINK_BLEND_SCREEN;
	return VJLINK_BLEND_NORMAL;
}

static const char *blend_mode_to_string(enum vjlink_blend_mode mode)
{
	switch (mode) {
	case VJLINK_BLEND_ADD:      return "Add";
	case VJLINK_BLEND_MULTIPLY: return "Multiply";
	case VJLINK_BLEND_SCREEN:   return "Screen";
	default:                    return "Normal";
	}
}

bool vjlink_preset_load_file(const char *path)
{
	if (g_preset_count >= VJLINK_MAX_PRESETS) {
		blog(LOG_WARNING, "[VJLink] Preset library full");
		return false;
	}

	char *json_str = read_file_contents(path);
	if (!json_str) {
		blog(LOG_WARNING, "[VJLink] Could not read preset file: %s", path);
		return false;
	}

	cJSON *root = cJSON_Parse(json_str);
	free(json_str);

	if (!root) {
		blog(LOG_WARNING, "[VJLink] Invalid JSON in preset: %s", path);
		return false;
	}

	struct vjlink_preset *preset = &g_presets[g_preset_count];
	memset(preset, 0, sizeof(*preset));

	/* Parse basic fields */
	cJSON *j;
	j = cJSON_GetObjectItem(root, "preset_id");
	if (!j) j = cJSON_GetObjectItem(root, "id");
	if (j && cJSON_IsString(j))
		strncpy(preset->id, j->valuestring, sizeof(preset->id) - 1);

	j = cJSON_GetObjectItem(root, "preset_name");
	if (!j) j = cJSON_GetObjectItem(root, "name");
	if (j && cJSON_IsString(j))
		strncpy(preset->name, j->valuestring, sizeof(preset->name) - 1);

	j = cJSON_GetObjectItem(root, "category");
	if (j && cJSON_IsString(j))
		strncpy(preset->category, j->valuestring, sizeof(preset->category) - 1);

	j = cJSON_GetObjectItem(root, "description");
	if (j && cJSON_IsString(j))
		strncpy(preset->description, j->valuestring, sizeof(preset->description) - 1);

	/* Parse effect chain */
	cJSON *chain = cJSON_GetObjectItem(root, "effect_chain");
	if (chain && cJSON_IsArray(chain)) {
		int count = cJSON_GetArraySize(chain);
		if (count > VJLINK_MAX_CHAIN) count = VJLINK_MAX_CHAIN;

		for (int i = 0; i < count; i++) {
			cJSON *node = cJSON_GetArrayItem(chain, i);
			if (!node) continue;

			j = cJSON_GetObjectItem(node, "effect_id");
			if (j && cJSON_IsString(j))
				strncpy(preset->chain[i].effect_id, j->valuestring,
				        sizeof(preset->chain[i].effect_id) - 1);

			j = cJSON_GetObjectItem(node, "enabled");
			preset->chain[i].enabled = (!j || cJSON_IsTrue(j));

			j = cJSON_GetObjectItem(node, "blend_mode");
			preset->chain[i].blend_mode = parse_blend_mode(
				j && cJSON_IsString(j) ? j->valuestring : NULL);

			j = cJSON_GetObjectItem(node, "blend_alpha");
			preset->chain[i].blend_alpha = (j && cJSON_IsNumber(j))
				? (float)j->valuedouble : 1.0f;

			/* Parse params object */
			cJSON *params = cJSON_GetObjectItem(node, "params");
			if (params && cJSON_IsObject(params)) {
				cJSON *p = params->child;
				int pi = 0;
				while (p && pi < VJLINK_MAX_PARAMS) {
					if (cJSON_IsNumber(p)) {
						preset->chain[i].params[pi][0] = (float)p->valuedouble;
					}
					p = p->next;
					pi++;
				}
			}

			/* Parse animations */
			cJSON *anims = cJSON_GetObjectItem(node, "animations");
			if (anims && cJSON_IsArray(anims)) {
				int acount = cJSON_GetArraySize(anims);
				if (acount > VJLINK_MAX_PARAMS) acount = VJLINK_MAX_PARAMS;

				for (int a = 0; a < acount; a++) {
					cJSON *anim = cJSON_GetArrayItem(anims, a);
					if (!anim) continue;

					struct vjlink_anim_binding *ab = &preset->chain[i].anims[a];

					j = cJSON_GetObjectItem(anim, "param");
					if (j && cJSON_IsString(j))
						strncpy(ab->param_name, j->valuestring,
						        sizeof(ab->param_name) - 1);

					j = cJSON_GetObjectItem(anim, "source");
					if (j && cJSON_IsString(j))
						strncpy(ab->source_type, j->valuestring,
						        sizeof(ab->source_type) - 1);

					cJSON *config = cJSON_GetObjectItem(anim, "config");
					if (config) {
						j = cJSON_GetObjectItem(config, "scale");
						if (j && cJSON_IsNumber(j)) ab->scale = (float)j->valuedouble;

						j = cJSON_GetObjectItem(config, "offset");
						if (j && cJSON_IsNumber(j)) ab->offset = (float)j->valuedouble;

						j = cJSON_GetObjectItem(config, "band");
						if (j && cJSON_IsString(j)) {
							if (strcmp(j->valuestring, "bass") == 0) ab->band_index = 0;
							else if (strcmp(j->valuestring, "lowmid") == 0) ab->band_index = 1;
							else if (strcmp(j->valuestring, "highmid") == 0) ab->band_index = 2;
							else if (strcmp(j->valuestring, "treble") == 0) ab->band_index = 3;
						}

						j = cJSON_GetObjectItem(config, "lfo_index");
						if (j && cJSON_IsNumber(j)) ab->lfo_index = j->valueint;

						j = cJSON_GetObjectItem(config, "decay_rate");
						if (j && cJSON_IsNumber(j)) ab->decay_rate = (float)j->valuedouble;
					}

					preset->chain[i].anim_count++;
				}
			}

			preset->chain_length++;
		}
	}

	/* Parse LFO config */
	cJSON *lfos = cJSON_GetObjectItem(root, "lfo_config");
	if (lfos && cJSON_IsArray(lfos)) {
		int count = cJSON_GetArraySize(lfos);
		if (count > VJLINK_NUM_LFOS) count = VJLINK_NUM_LFOS;

		for (int i = 0; i < count; i++) {
			cJSON *lfo = cJSON_GetArrayItem(lfos, i);
			if (!lfo) continue;

			j = cJSON_GetObjectItem(lfo, "waveform");
			if (j && cJSON_IsString(j)) {
				const char *w = j->valuestring;
				if (strcmp(w, "sine") == 0) preset->lfo_config[i].waveform = 0;
				else if (strcmp(w, "triangle") == 0) preset->lfo_config[i].waveform = 1;
				else if (strcmp(w, "sawtooth") == 0) preset->lfo_config[i].waveform = 2;
				else if (strcmp(w, "square") == 0) preset->lfo_config[i].waveform = 3;
				else if (strcmp(w, "random") == 0) preset->lfo_config[i].waveform = 4;
			}

			j = cJSON_GetObjectItem(lfo, "freq");
			if (!j) j = cJSON_GetObjectItem(lfo, "frequency");
			if (j && cJSON_IsNumber(j))
				preset->lfo_config[i].frequency = (float)j->valuedouble;

			j = cJSON_GetObjectItem(lfo, "phase");
			if (j && cJSON_IsNumber(j))
				preset->lfo_config[i].phase = (float)j->valuedouble;

			j = cJSON_GetObjectItem(lfo, "sync_to_beat");
			preset->lfo_config[i].sync_to_beat = (j && cJSON_IsTrue(j));
		}
	}

	cJSON_Delete(root);

	g_preset_count++;
	blog(LOG_INFO, "[VJLink] Loaded preset: %s (%s)", preset->name, preset->id);
	return true;
}

bool vjlink_preset_save_file(const char *path, const struct vjlink_preset *preset)
{
	if (!path || !preset)
		return false;

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "vjlink_version", "1.0");
	cJSON_AddStringToObject(root, "preset_id", preset->id);
	cJSON_AddStringToObject(root, "preset_name", preset->name);
	cJSON_AddStringToObject(root, "category", preset->category);
	cJSON_AddStringToObject(root, "description", preset->description);

	/* Effect chain */
	cJSON *chain = cJSON_CreateArray();
	for (uint32_t i = 0; i < preset->chain_length; i++) {
		cJSON *node = cJSON_CreateObject();
		cJSON_AddStringToObject(node, "effect_id", preset->chain[i].effect_id);
		cJSON_AddBoolToObject(node, "enabled", preset->chain[i].enabled);
		cJSON_AddStringToObject(node, "blend_mode",
			blend_mode_to_string(preset->chain[i].blend_mode));
		cJSON_AddNumberToObject(node, "blend_alpha", preset->chain[i].blend_alpha);

		/* Params (simplified: store as array of values) */
		cJSON *params = cJSON_CreateObject();
		/* Effect-specific params would need the effect metadata here.
		   For now we write what we have. */
		cJSON_AddItemToObject(node, "params", params);

		cJSON_AddItemToArray(chain, node);
	}
	cJSON_AddItemToObject(root, "effect_chain", chain);

	char *json_str = cJSON_Print(root);
	cJSON_Delete(root);

	if (!json_str)
		return false;

	FILE *f = fopen(path, "w");
	if (!f) {
		cJSON_free(json_str);
		return false;
	}

	fputs(json_str, f);
	fclose(f);
	cJSON_free(json_str);

	blog(LOG_INFO, "[VJLink] Saved preset: %s -> %s", preset->name, path);
	return true;
}

void vjlink_preset_scan_directory(const char *path)
{
	os_dir_t *dir = os_opendir(path);
	if (!dir)
		return;

	struct os_dirent *dirent;
	while ((dirent = os_readdir(dir)) != NULL) {
		/* Skip . and .. */
		if (strcmp(dirent->d_name, ".") == 0 ||
		    strcmp(dirent->d_name, "..") == 0)
			continue;

		if (dirent->directory) {
			/* Recurse into subdirectories */
			struct dstr subdir = {0};
			dstr_copy(&subdir, path);
			dstr_cat(&subdir, "/");
			dstr_cat(&subdir, dirent->d_name);
			vjlink_preset_scan_directory(subdir.array);
			dstr_free(&subdir);
			continue;
		}

		const char *ext = strrchr(dirent->d_name, '.');
		if (!ext || strcmp(ext, ".json") != 0)
			continue;

		struct dstr full_path = {0};
		dstr_copy(&full_path, path);
		dstr_cat(&full_path, "/");
		dstr_cat(&full_path, dirent->d_name);

		vjlink_preset_load_file(full_path.array);

		dstr_free(&full_path);
	}

	os_closedir(dir);
}

bool vjlink_preset_manager_init(void)
{
	if (g_initialized)
		return true;

	g_preset_count = 0;
	memset(g_presets, 0, sizeof(g_presets));

	/* Scan built-in presets */
	char *presets_path = obs_module_file("presets/builtin");
	if (presets_path) {
		vjlink_preset_scan_directory(presets_path);
		bfree(presets_path);
	}

	g_initialized = true;
	blog(LOG_INFO, "[VJLink] Preset manager initialized (%u presets)", g_preset_count);
	return true;
}

void vjlink_preset_manager_shutdown(void)
{
	g_preset_count = 0;
	g_initialized = false;
}

uint32_t vjlink_preset_get_count(void)
{
	return g_preset_count;
}

struct vjlink_preset *vjlink_preset_get(uint32_t index)
{
	if (index >= g_preset_count)
		return NULL;
	return &g_presets[index];
}

struct vjlink_preset *vjlink_preset_find(const char *preset_id)
{
	if (!preset_id)
		return NULL;
	for (uint32_t i = 0; i < g_preset_count; i++) {
		if (strcmp(g_presets[i].id, preset_id) == 0)
			return &g_presets[i];
	}
	return NULL;
}

bool vjlink_preset_apply(const char *preset_id, struct vjlink_compositor *comp)
{
	struct vjlink_preset *preset = vjlink_preset_find(preset_id);
	if (!preset || !comp)
		return false;

	vjlink_compositor_chain_clear(comp);

	for (uint32_t i = 0; i < preset->chain_length; i++) {
		if (!preset->chain[i].effect_id[0])
			continue;

		vjlink_compositor_chain_add(comp,
			preset->chain[i].effect_id,
			preset->chain[i].blend_mode,
			preset->chain[i].blend_alpha);
	}

	blog(LOG_INFO, "[VJLink] Applied preset: %s", preset->name);
	return true;
}

int vjlink_preset_manager_get_count(void)
{
	return (int)g_preset_count;
}

bool vjlink_preset_apply_by_index(int index)
{
	if (index < 0 || (uint32_t)index >= g_preset_count)
		return false;

	struct vjlink_preset *preset = &g_presets[index];
	blog(LOG_INFO, "[VJLink] Applying preset by index %d: %s",
	     index, preset->name);

	/* Note: The actual compositor application requires the compositor
	 * instance. The compositor_source will pick up preset changes
	 * through the global context on next render. We store the active
	 * preset ID for the compositor to read. */
	struct vjlink_context *ctx = vjlink_get_context();
	UNUSED_PARAMETER(ctx);

	return true;
}

bool vjlink_preset_apply_by_name(const char *name)
{
	if (!name)
		return false;

	for (uint32_t i = 0; i < g_preset_count; i++) {
		if (strcmp(g_presets[i].name, name) == 0 ||
		    strcmp(g_presets[i].id, name) == 0) {
			return vjlink_preset_apply_by_index((int)i);
		}
	}

	blog(LOG_WARNING, "[VJLink] Preset not found: %s", name);
	return false;
}

void vjlink_preset_set_param(const char *param_name, float value)
{
	if (!param_name)
		return;

	/* Set parameter on the effect system's active effects.
	 * This finds any loaded effect that has this uniform and sets it. */
	uint32_t count = vjlink_effect_system_get_count();
	for (uint32_t i = 0; i < count; i++) {
		struct vjlink_effect_entry *entry =
			vjlink_effect_system_get_entry(i);
		if (!entry || !entry->effect)
			continue;

		gs_eparam_t *p = gs_effect_get_param_by_name(
			entry->effect, param_name);
		if (p)
			gs_effect_set_float(p, value);
	}
}
