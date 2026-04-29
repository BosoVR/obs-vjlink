#include "effect_system.h"
#include "audio/audio_texture.h"
#include "presets/cjson/cJSON.h"
#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#define os_stat _stat
#define os_stat_t struct _stat
#else
#include <sys/stat.h>
#define os_stat stat
#define os_stat_t struct stat
#endif

/* Global effect registry */
static struct vjlink_effect_entry g_effects[VJLINK_MAX_REGISTERED_EFFECTS];
static uint32_t g_effect_count = 0;
static bool g_initialized = false;

/* 1x1 fallback texture so unbound texture2d params don't block draws */
static gs_texture_t *g_fallback_tex = NULL;

/* Effect category subdirectories to scan */
static const char *effect_categories[] = {
	"tunnel", "plasma", "particles", "fractal",
	"geometric", "glitch", "retro", "audio_viz",
	"postprocess", "3d", "flash", "common",
	NULL
};

static void register_effect(const char *id, const char *name,
                            const char *category, const char *path)
{
	if (g_effect_count >= VJLINK_MAX_REGISTERED_EFFECTS) {
		blog(LOG_WARNING, "[VJLink] Effect registry full, skipping: %s", id);
		return;
	}

	struct vjlink_effect_entry *entry = &g_effects[g_effect_count];
	memset(entry, 0, sizeof(*entry));

	strncpy(entry->id, id, sizeof(entry->id) - 1);
	strncpy(entry->name, name, sizeof(entry->name) - 1);
	strncpy(entry->category, category, sizeof(entry->category) - 1);
	strncpy(entry->effect_path, path, sizeof(entry->effect_path) - 1);

	g_effect_count++;
	blog(LOG_INFO, "[VJLink] Registered effect: %s (%s) [%s]", id, name, category);
}

static enum vjlink_param_type parse_param_type(const char *type_str)
{
	if (!type_str) return VJLINK_PARAM_FLOAT;
	if (strcmp(type_str, "float") == 0) return VJLINK_PARAM_FLOAT;
	if (strcmp(type_str, "int") == 0) return VJLINK_PARAM_INT;
	if (strcmp(type_str, "bool") == 0) return VJLINK_PARAM_BOOL;
	if (strcmp(type_str, "color") == 0) return VJLINK_PARAM_COLOR;
	if (strcmp(type_str, "vec2") == 0) return VJLINK_PARAM_VEC2;
	if (strcmp(type_str, "vec4") == 0) return VJLINK_PARAM_VEC4;
	return VJLINK_PARAM_FLOAT;
}

static void load_effect_metadata(struct vjlink_effect_entry *entry)
{
	/* Build path: <module_data>/effects_meta/<id>.json
	 * For flash effects, try effects_meta/flash/<id>.json first */
	char *base_path = obs_module_file("effects_meta");
	if (!base_path)
		return;

	struct dstr json_path = {0};

	/* Try category subdirectory first (for flash effects) */
	dstr_printf(&json_path, "%s/%s/%s.json", base_path, entry->category, entry->id);

	char *json_data = os_quick_read_utf8_file(json_path.array);
	if (!json_data) {
		/* Try flat directory */
		dstr_printf(&json_path, "%s/%s.json", base_path, entry->id);
		json_data = os_quick_read_utf8_file(json_path.array);
	}

	dstr_free(&json_path);
	bfree(base_path);

	if (!json_data)
		return;

	cJSON *root = cJSON_Parse(json_data);
	bfree(json_data);
	if (!root)
		return;

	/* Override display name if present */
	cJSON *name_item = cJSON_GetObjectItem(root, "name");
	if (name_item && name_item->valuestring)
		strncpy(entry->name, name_item->valuestring,
		        sizeof(entry->name) - 1);

	/* Parse parameters */
	cJSON *params = cJSON_GetObjectItem(root, "params");
	if (params && cJSON_IsArray(params)) {
		int count = cJSON_GetArraySize(params);
		if (count > VJLINK_MAX_PARAMS)
			count = VJLINK_MAX_PARAMS;

		for (int i = 0; i < count; i++) {
			cJSON *p = cJSON_GetArrayItem(params, i);
			if (!p) continue;

			struct vjlink_param_def *def = &entry->params[i];

			cJSON *pid = cJSON_GetObjectItem(p, "id");
			cJSON *pname = cJSON_GetObjectItem(p, "name");
			cJSON *plabel = cJSON_GetObjectItem(p, "label");
			cJSON *ptype = cJSON_GetObjectItem(p, "type");
			cJSON *pdefault = cJSON_GetObjectItem(p, "default");
			cJSON *pmin = cJSON_GetObjectItem(p, "min");
			cJSON *pmax = cJSON_GetObjectItem(p, "max");
			cJSON *pstep = cJSON_GetObjectItem(p, "step");

			/*
			 * Handle two JSON schemas:
			 *   Schema A: "id"=uniform, "name"=display
			 *   Schema B: "name"=uniform, "label"=display
			 * Prefer "id" for uniform name (Schema A),
			 * fall back to "name" (Schema B).
			 */
			const char *uniform_name = NULL;
			const char *display_label = NULL;

			if (pid && pid->valuestring) {
				/* Schema A */
				uniform_name = pid->valuestring;
				display_label = (pname && pname->valuestring)
					? pname->valuestring : pid->valuestring;
			} else if (pname && pname->valuestring) {
				/* Schema B */
				uniform_name = pname->valuestring;
				display_label = (plabel && plabel->valuestring)
					? plabel->valuestring : pname->valuestring;
			}

			if (uniform_name)
				strncpy(def->name, uniform_name,
				        sizeof(def->name) - 1);
			if (display_label)
				strncpy(def->label, display_label,
				        sizeof(def->label) - 1);

			def->type = parse_param_type(
				ptype ? ptype->valuestring : NULL);

			if (pdefault)
				def->default_val[0] = (float)pdefault->valuedouble;
			if (pmin)
				def->min_val = (float)pmin->valuedouble;
			if (pmax)
				def->max_val = (float)pmax->valuedouble;
			if (pstep)
				def->step = (float)pstep->valuedouble;
		}
		entry->param_count = (uint32_t)count;
	}

	cJSON_Delete(root);
	blog(LOG_INFO, "[VJLink] Loaded metadata for '%s': %u params",
	     entry->id, entry->param_count);
}

static void scan_effects_directory(void)
{
	char *base_path = obs_module_file("effects");
	if (!base_path) {
		blog(LOG_ERROR, "[VJLink] obs_module_file(\"effects\") returned NULL - "
		     "data directory not found!");
		return;
	}
	blog(LOG_INFO, "[VJLink] Scanning effects base path: %s", base_path);

	for (int c = 0; effect_categories[c]; c++) {
		const char *cat = effect_categories[c];

		/* Build path: <module_data>/effects/<category>/ */
		struct dstr dir_path = {0};

		dstr_copy(&dir_path, base_path);
		dstr_cat(&dir_path, "/");
		dstr_cat(&dir_path, cat);

		os_dir_t *dir = os_opendir(dir_path.array);
		if (!dir) {
			blog(LOG_DEBUG, "[VJLink] Category dir not found: %s",
			     dir_path.array);
			dstr_free(&dir_path);
			continue;
		}

		struct os_dirent *dirent;
		while ((dirent = os_readdir(dir)) != NULL) {
			/* Only .effect files */
			const char *ext = strrchr(dirent->d_name, '.');
			if (!ext || strcmp(ext, ".effect") != 0)
				continue;

			/* Include all effects, even vjlink_test */

			/* Extract ID from filename (without extension) */
			char id[64];
			size_t name_len = ext - dirent->d_name;
			if (name_len >= sizeof(id)) name_len = sizeof(id) - 1;
			memcpy(id, dirent->d_name, name_len);
			id[name_len] = '\0';

			/* Build full path */
			struct dstr full_path = {0};
			dstr_copy(&full_path, dir_path.array);
			dstr_cat(&full_path, "/");
			dstr_cat(&full_path, dirent->d_name);

			/* Register and load metadata (name, params, defaults) */
			register_effect(id, id, cat, full_path.array);
			load_effect_metadata(&g_effects[g_effect_count - 1]);

			/* Hide effects that lack a "Draw" technique
			 * (multi-pass sim shaders, utility blits) */
			if (strcmp(id, "particle_sim") == 0 ||
			    strcmp(id, "particle_render") == 0 ||
			    strcmp(id, "videowall_blit") == 0 ||
			    strcmp(id, "luma_alpha") == 0 ||
			    strcmp(id, "debug_overlay") == 0 ||
			    strcmp(id, "dna_helix") == 0) {
				g_effects[g_effect_count - 1].hidden = true;
			}

			dstr_free(&full_path);
		}

		os_closedir(dir);
		dstr_free(&dir_path);
	}

	bfree(base_path);
}

static void create_fallback_texture(void)
{
	if (g_fallback_tex)
		return;

	/* 1x1 opaque black pixel */
	uint32_t pixel = 0xFF000000;
	const uint8_t *pixel_ptr = (const uint8_t *)&pixel;
	g_fallback_tex = gs_texture_create(1, 1, GS_RGBA, 1,
	                                    &pixel_ptr, 0);
	if (g_fallback_tex)
		blog(LOG_INFO, "[VJLink] Fallback texture created");
}

bool vjlink_effect_system_init(void)
{
	if (g_initialized)
		return true;

	g_effect_count = 0;
	memset(g_effects, 0, sizeof(g_effects));

	scan_effects_directory();

	g_initialized = true;
	blog(LOG_INFO, "[VJLink] Effect system initialized (%u effects registered)",
	     g_effect_count);
	return true;
}

void vjlink_effect_system_shutdown(void)
{
	if (!g_initialized)
		return;

	/*
	 * During obs_shutdown -> free_module, the graphics subsystem may
	 * already be torn down. Attempting to enter graphics or destroy
	 * GPU resources causes access violations. OBS cleans up all GPU
	 * resources automatically, so we just NULL our pointers.
	 */
	for (uint32_t i = 0; i < g_effect_count; i++) {
		g_effects[i].effect = NULL;
		g_effects[i].loaded = false;
	}
	g_fallback_tex = NULL;

	g_effect_count = 0;
	g_initialized = false;
	blog(LOG_INFO, "[VJLink] Effect system shutdown");
}

uint32_t vjlink_effect_system_get_count(void)
{
	return g_effect_count;
}

struct vjlink_effect_entry *vjlink_effect_system_get_entry(uint32_t index)
{
	if (index >= g_effect_count)
		return NULL;
	return &g_effects[index];
}

struct vjlink_effect_entry *vjlink_effect_system_find(const char *effect_id)
{
	if (!effect_id)
		return NULL;

	for (uint32_t i = 0; i < g_effect_count; i++) {
		if (strcmp(g_effects[i].id, effect_id) == 0)
			return &g_effects[i];
	}
	return NULL;
}

static void cache_standard_params(struct vjlink_effect_entry *entry)
{
	gs_effect_t *e = entry->effect;
	entry->p_view_proj  = gs_effect_get_param_by_name(e, "ViewProj");
	entry->p_audio_tex  = gs_effect_get_param_by_name(e, "audio_tex");
	entry->p_image      = gs_effect_get_param_by_name(e, "image");
	entry->p_input_tex  = gs_effect_get_param_by_name(e, "input_tex");
	entry->p_prev_tex   = gs_effect_get_param_by_name(e, "prev_tex");
	entry->p_resolution = gs_effect_get_param_by_name(e, "resolution");
	entry->p_time       = gs_effect_get_param_by_name(e, "time");
	entry->p_bands      = gs_effect_get_param_by_name(e, "bands");
	entry->p_bands_raw  = gs_effect_get_param_by_name(e, "bands_raw");
	entry->p_chronotensity = gs_effect_get_param_by_name(e, "chronotensity");
	entry->p_beat_phase = gs_effect_get_param_by_name(e, "beat_phase");
	entry->p_bpm        = gs_effect_get_param_by_name(e, "bpm");
	entry->p_beat_confidence = gs_effect_get_param_by_name(e, "beat_confidence");
	entry->p_onset_strength = gs_effect_get_param_by_name(e, "onset_strength");
	entry->p_rms        = gs_effect_get_param_by_name(e, "rms");
	entry->p_kick_onset = gs_effect_get_param_by_name(e, "kick_onset");
	entry->p_snare_onset = gs_effect_get_param_by_name(e, "snare_onset");
	entry->p_hat_onset  = gs_effect_get_param_by_name(e, "hat_onset");
	entry->p_beat_1_4   = gs_effect_get_param_by_name(e, "beat_1_4");
	entry->p_beat_1_8   = gs_effect_get_param_by_name(e, "beat_1_8");
	entry->p_beat_1_16  = gs_effect_get_param_by_name(e, "beat_1_16");
	entry->p_beat_2_1   = gs_effect_get_param_by_name(e, "beat_2_1");
	entry->p_beat_4_1   = gs_effect_get_param_by_name(e, "beat_4_1");
	entry->p_beat_count = gs_effect_get_param_by_name(e, "beat_count");
	entry->p_palette_id = gs_effect_get_param_by_name(e, "palette_id");
	entry->p_quality    = gs_effect_get_param_by_name(e, "quality");
	entry->p_band_activation = gs_effect_get_param_by_name(e, "band_activation");
	entry->p_has_input = gs_effect_get_param_by_name(e, "has_input_source");
	entry->p_logo_tex  = gs_effect_get_param_by_name(e, "logo_tex");
	entry->p_logo_tex2 = gs_effect_get_param_by_name(e, "logo_tex2");
	entry->p_logo_tex3 = gs_effect_get_param_by_name(e, "logo_tex3");

	/* Cache custom parameter handles */
	for (uint32_t i = 0; i < entry->param_count; i++) {
		entry->param_handles[i] = gs_effect_get_param_by_name(
			e, entry->params[i].name);
	}
}

bool vjlink_effect_ensure_loaded(struct vjlink_effect_entry *entry)
{
	if (!entry)
		return false;

	if (entry->loaded && entry->effect)
		return true;

	/* Compile the effect from file */
	blog(LOG_INFO, "[VJLink] Compiling effect '%s' from: %s",
	     entry->id, entry->effect_path);

	char *errors = NULL;
	entry->effect = gs_effect_create_from_file(entry->effect_path, &errors);

	if (!entry->effect) {
		const char *err = errors ? errors : "unknown error";
		blog(LOG_ERROR, "[VJLink] FAILED to compile effect '%s' "
		     "path='%s' error='%s'",
		     entry->id, entry->effect_path, err);

		/* Store error in global context for UI display */
		struct vjlink_context *ctx = vjlink_get_context();
		snprintf(ctx->last_error, sizeof(ctx->last_error),
		         "Effect '%s': %s", entry->id, err);
		ctx->has_error = true;

		/* Append to ring buffer for Web UI Diagnostics panel */
		char err_short[256];
		snprintf(err_short, sizeof(err_short), "%s: %.200s", entry->id, err);
		vjlink_context_log_shader_error(err_short);

		bfree(errors);
		return false;
	}

	if (errors) {
		blog(LOG_WARNING, "[VJLink] Effect '%s' compiled with warnings: %s",
		     entry->id, errors);
		bfree(errors);
	}

	cache_standard_params(entry);
	entry->loaded = true;

	/* Store file modification time for hot-reload */
	{
		os_stat_t st;
		if (os_stat(entry->effect_path, &st) == 0)
			entry->file_mtime = st.st_mtime;
	}

	blog(LOG_INFO, "[VJLink] Effect '%s' compiled successfully", entry->id);
	return true;
}

void vjlink_effect_bind_uniforms(struct vjlink_effect_entry *entry,
                                 gs_texture_t *input_tex,
                                 gs_texture_t *prev_tex,
                                 uint32_t width, uint32_t height)
{
	if (!entry || !entry->effect)
		return;

	struct vjlink_context *ctx = vjlink_get_context();

	/* Ensure fallback texture exists */
	if (!g_fallback_tex)
		create_fallback_texture();

	/*
	 * CRITICAL: Set defaults for ALL shader params before draw.
	 * D3D11 skips the entire draw call if any param is unbound
	 * ("Not all shader parameters were set").
	 * Pattern from obs-shaderfilter: enumerate all params, set
	 * type-appropriate defaults, skip ViewProj (auto-bound).
	 */
	{
		size_t num_params = gs_effect_get_num_params(entry->effect);
		for (size_t i = 0; i < num_params; i++) {
			gs_eparam_t *param = gs_effect_get_param_by_idx(
				entry->effect, i);
			struct gs_effect_param_info info;
			gs_effect_get_param_info(param, &info);

			/* ViewProj is auto-bound by gs_effect_loop */
			if (strcmp(info.name, "ViewProj") == 0)
				continue;

			switch (info.type) {
			case GS_SHADER_PARAM_TEXTURE:
				if (g_fallback_tex)
					gs_effect_set_texture(param,
						g_fallback_tex);
				break;
			case GS_SHADER_PARAM_FLOAT:
				gs_effect_set_float(param, 0.0f);
				break;
			case GS_SHADER_PARAM_INT:
				gs_effect_set_int(param, 0);
				break;
			case GS_SHADER_PARAM_BOOL:
				gs_effect_set_bool(param, false);
				break;
			case GS_SHADER_PARAM_VEC2: {
				struct vec2 z;
				vec2_zero(&z);
				gs_effect_set_vec2(param, &z);
				break;
			}
			case GS_SHADER_PARAM_VEC3: {
				struct vec4 z;
				vec4_zero(&z);
				gs_effect_set_val(param, &z, sizeof(float) * 3);
				break;
			}
			case GS_SHADER_PARAM_VEC4: {
				struct vec4 z;
				vec4_zero(&z);
				gs_effect_set_vec4(param, &z);
				break;
			}
			default:
				break;
			}
		}
	}

	/* Now bind actual textures (overriding fallbacks) */

	/* Audio texture */
	if (entry->p_audio_tex) {
		gs_texture_t *audio_tex = vjlink_audio_texture_get();
		if (audio_tex)
			gs_effect_set_texture(entry->p_audio_tex, audio_tex);
	}

	/* Input texture - bind to both "image" and "input_tex" uniforms.
	 * Most filter effects (VHS, RGB Split, etc.) use "image",
	 * while some post-process effects use "input_tex". */
	if (input_tex) {
		if (entry->p_image)
			gs_effect_set_texture(entry->p_image, input_tex);
		if (entry->p_input_tex)
			gs_effect_set_texture(entry->p_input_tex, input_tex);
	}

	/* Previous frame (feedback) */
	if (entry->p_prev_tex && prev_tex)
		gs_effect_set_texture(entry->p_prev_tex, prev_tex);

	/* Resolution */
	if (entry->p_resolution) {
		struct vec2 res;
		vec2_set(&res, (float)width, (float)height);
		gs_effect_set_vec2(entry->p_resolution, &res);
	}

	/* Time */
	if (entry->p_time)
		gs_effect_set_float(entry->p_time, ctx->elapsed_time);

	/* Bands */
	if (entry->p_bands) {
		struct vec4 bands;
		vec4_set(&bands,
			ctx->bands[0], ctx->bands[1],
			ctx->bands[2], ctx->bands[3]);
		gs_effect_set_vec4(entry->p_bands, &bands);
	}
	if (entry->p_bands_raw) {
		struct vec4 bands_raw;
		vec4_set(&bands_raw,
			ctx->bands_raw[0], ctx->bands_raw[1],
			ctx->bands_raw[2], ctx->bands_raw[3]);
		gs_effect_set_vec4(entry->p_bands_raw, &bands_raw);
	}
	if (entry->p_chronotensity) {
		struct vec4 chrono;
		vec4_set(&chrono,
			ctx->chronotensity[0], ctx->chronotensity[1],
			ctx->chronotensity[2], ctx->chronotensity[3]);
		gs_effect_set_vec4(entry->p_chronotensity, &chrono);
	}

	/* Beat phase */
	if (entry->p_beat_phase)
		gs_effect_set_float(entry->p_beat_phase, ctx->beat_phase);

	/* BPM */
	if (entry->p_bpm)
		gs_effect_set_float(entry->p_bpm, ctx->bpm);
	if (entry->p_beat_confidence)
		gs_effect_set_float(entry->p_beat_confidence, ctx->beat_confidence);
	if (entry->p_onset_strength)
		gs_effect_set_float(entry->p_onset_strength, ctx->onset_strength);
	if (entry->p_rms)
		gs_effect_set_float(entry->p_rms, ctx->rms);
	if (entry->p_kick_onset)
		gs_effect_set_float(entry->p_kick_onset, ctx->kick_onset);
	if (entry->p_snare_onset)
		gs_effect_set_float(entry->p_snare_onset, ctx->snare_onset);
	if (entry->p_hat_onset)
		gs_effect_set_float(entry->p_hat_onset, ctx->hat_onset);
	if (entry->p_beat_1_4)
		gs_effect_set_float(entry->p_beat_1_4, ctx->beat_1_4);
	if (entry->p_beat_1_8)
		gs_effect_set_float(entry->p_beat_1_8, ctx->beat_1_8);
	if (entry->p_beat_1_16)
		gs_effect_set_float(entry->p_beat_1_16, ctx->beat_1_16);
	if (entry->p_beat_2_1)
		gs_effect_set_float(entry->p_beat_2_1, ctx->beat_2_1);
	if (entry->p_beat_4_1)
		gs_effect_set_float(entry->p_beat_4_1, ctx->beat_4_1);
	if (entry->p_beat_count)
		gs_effect_set_float(entry->p_beat_count, (float)(ctx->beat_count & 0xFFFF));
	if (entry->p_palette_id)
		gs_effect_set_float(entry->p_palette_id, (float)ctx->palette_id);

	/* GPU quality level (0=low, 1=medium, 2=high) */
	if (entry->p_quality)
		gs_effect_set_float(entry->p_quality, (float)ctx->gpu_quality);

	/* Logo textures (up to 3 user-selected images) */
	if (entry->p_logo_tex && ctx->logo_texture)
		gs_effect_set_texture(entry->p_logo_tex, ctx->logo_texture);
	if (entry->p_logo_tex2 && ctx->logo_texture2)
		gs_effect_set_texture(entry->p_logo_tex2, ctx->logo_texture2);
	if (entry->p_logo_tex3 && ctx->logo_texture3)
		gs_effect_set_texture(entry->p_logo_tex3, ctx->logo_texture3);
}

void vjlink_effect_bind_custom_params(struct vjlink_effect_entry *entry,
                                       const float values[][4])
{
	if (!entry)
		return;

	for (uint32_t p = 0; p < entry->param_count; p++) {
		gs_eparam_t *handle = entry->param_handles[p];
		if (!handle)
			continue;

		const float *v = values ? values[p] : entry->params[p].default_val;

		switch (entry->params[p].type) {
		case VJLINK_PARAM_FLOAT:
		case VJLINK_PARAM_INT:
		case VJLINK_PARAM_BOOL:
			gs_effect_set_float(handle, v[0]);
			break;
		case VJLINK_PARAM_VEC2: {
			struct vec2 vec;
			vec2_set(&vec, v[0], v[1]);
			gs_effect_set_vec2(handle, &vec);
			break;
		}
		case VJLINK_PARAM_COLOR:
		case VJLINK_PARAM_VEC4: {
			struct vec4 vec;
			vec4_set(&vec, v[0], v[1], v[2], v[3]);
			gs_effect_set_vec4(handle, &vec);
			break;
		}
		}
	}
}

void vjlink_effect_set_param_float(struct vjlink_effect_entry *entry,
                                   const char *name, float value)
{
	if (!entry || !entry->effect || !name)
		return;

	gs_eparam_t *param = gs_effect_get_param_by_name(entry->effect, name);
	if (param)
		gs_effect_set_float(param, value);
}

void vjlink_effect_set_param_vec4(struct vjlink_effect_entry *entry,
                                  const char *name, const struct vec4 *value)
{
	if (!entry || !entry->effect || !name || !value)
		return;

	gs_eparam_t *param = gs_effect_get_param_by_name(entry->effect, name);
	if (param)
		gs_effect_set_vec4(param, value);
}

bool vjlink_effect_check_hot_reload(struct vjlink_effect_entry *entry)
{
	if (!entry || !entry->loaded || !entry->effect_path[0])
		return false;

	os_stat_t st;
	if (os_stat(entry->effect_path, &st) != 0)
		return false;

	if (st.st_mtime <= entry->file_mtime)
		return false;

	/* File has been modified — recompile */
	blog(LOG_INFO, "[VJLink] Hot-reload: '%s' modified, recompiling...",
	     entry->id);

	if (entry->effect) {
		gs_effect_destroy(entry->effect);
		entry->effect = NULL;
	}
	entry->loaded = false;

	/* Clear cached param handles */
	entry->p_view_proj = NULL;
	entry->p_audio_tex = NULL;
	entry->p_image = NULL;
	entry->p_input_tex = NULL;
	entry->p_prev_tex = NULL;
	entry->p_resolution = NULL;
	entry->p_time = NULL;
	entry->p_bands = NULL;
	entry->p_bands_raw = NULL;
	entry->p_chronotensity = NULL;
	entry->p_beat_phase = NULL;
	entry->p_bpm = NULL;
	entry->p_beat_confidence = NULL;
	entry->p_onset_strength = NULL;
	entry->p_rms = NULL;
	entry->p_kick_onset = NULL;
	entry->p_snare_onset = NULL;
	entry->p_hat_onset = NULL;
	entry->p_beat_1_4 = NULL;
	entry->p_beat_1_8 = NULL;
	entry->p_beat_1_16 = NULL;
	entry->p_beat_2_1 = NULL;
	entry->p_beat_4_1 = NULL;
	entry->p_beat_count = NULL;
	entry->p_palette_id = NULL;
	entry->p_quality = NULL;
	entry->p_band_activation = NULL;
	entry->p_has_input = NULL;
	entry->p_logo_tex = NULL;
	entry->p_logo_tex2 = NULL;
	entry->p_logo_tex3 = NULL;
	memset(entry->param_handles, 0, sizeof(entry->param_handles));

	/* Reload */
	bool ok = vjlink_effect_ensure_loaded(entry);
	if (ok)
		blog(LOG_INFO, "[VJLink] Hot-reload: '%s' recompiled OK", entry->id);
	else
		blog(LOG_ERROR, "[VJLink] Hot-reload: '%s' FAILED", entry->id);

	return ok;
}
