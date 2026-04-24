#include "websocket_handler.h"
#include "hotkey_manager.h"
#include "vjlink_context.h"
#include "presets/preset_manager.h"
#include "rendering/band_effects.h"
#include "controls/source_trigger.h"
#include "rendering/media_layer.h"
#include <obs-module.h>
#include <string.h>
#include <stdio.h>

/*
 * VJLink WebSocket Handler
 *
 * Uses the obs-websocket vendor API to register custom request types.
 * The vendor API was introduced in obs-websocket v5 (OBS 28+).
 *
 * External clients connect via obs-websocket and send vendor requests:
 *   { "requestType": "CallVendorRequest",
 *     "requestData": {
 *       "vendorName": "obs-vjlink",
 *       "requestType": "SetPreset",
 *       "requestData": { "preset_name": "Neon Tunnel" }
 *     }
 *   }
 */

/*
 * obs-websocket vendor API
 *
 * obs-websocket v5 (OBS 28+) has its OWN proc handler, separate from the
 * global OBS proc handler. We must first fetch it via
 * "obs_websocket_api_get_ph" on the global handler, then use the returned
 * proc handler for all vendor operations.
 */

typedef void (*ws_request_cb_fn)(obs_data_t *, obs_data_t *, void *);

struct ws_request_callback {
	ws_request_cb_fn callback;
	void *priv_data;
};

static void *g_vendor = NULL;
static bool g_ws_available = false;
static proc_handler_t *g_ws_ph = NULL;

static proc_handler_t *get_ws_proc_handler(void)
{
	if (g_ws_ph)
		return g_ws_ph;

	proc_handler_t *global_ph = obs_get_proc_handler();
	if (!global_ph)
		return NULL;

	calldata_t cd = {0};
	if (!proc_handler_call(global_ph, "obs_websocket_api_get_ph", &cd)) {
		calldata_free(&cd);
		return NULL;
	}
	g_ws_ph = (proc_handler_t *)calldata_ptr(&cd, "ph");
	calldata_free(&cd);
	return g_ws_ph;
}

static void *vendor_register(const char *name)
{
	proc_handler_t *ph = get_ws_proc_handler();
	if (!ph)
		return NULL;

	calldata_t cd = {0};
	calldata_set_string(&cd, "name", name);
	proc_handler_call(ph, "vendor_register", &cd);
	void *vendor = calldata_ptr(&cd, "vendor");
	calldata_free(&cd);
	return vendor;
}

static bool vendor_request_register(void *vendor, const char *type,
	ws_request_cb_fn cb, void *priv)
{
	proc_handler_t *ph = get_ws_proc_handler();
	if (!ph)
		return false;

	struct ws_request_callback cb_data = {cb, priv};

	calldata_t cd = {0};
	calldata_set_ptr(&cd, "vendor", vendor);
	calldata_set_string(&cd, "type", type);
	calldata_set_ptr(&cd, "callback", &cb_data);

	proc_handler_call(ph, "vendor_request_register", &cd);
	bool success = calldata_bool(&cd, "success");
	calldata_free(&cd);
	return success;
}

static void vendor_request_unregister(void *vendor, const char *type)
{
	proc_handler_t *ph = get_ws_proc_handler();
	if (!ph)
		return;

	calldata_t cd = {0};
	calldata_set_ptr(&cd, "vendor", vendor);
	calldata_set_string(&cd, "type", type);
	proc_handler_call(ph, "vendor_request_unregister", &cd);
	calldata_free(&cd);
}

/* --- Request Handlers --- */

static void handle_set_preset(obs_data_t *request_data,
                               obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();
	const char *name = obs_data_get_string(request_data, "preset_name");

	if (!name || !*name) {
		/* Empty name = clear preset/effect */
		ctx->pending_effect[0] = '\0';
		ctx->effect_pending = true;
		ctx->active_preset_index = -1;
		vjlink_preset_set_index(-1); /* Clear hotkey index too */
		obs_data_set_bool(response_data, "success", true);
		blog(LOG_INFO, "[VJLink] WebSocket: preset/effect cleared");
		return;
	}

	/* Find preset and set its first effect on the compositor */
	bool ok = vjlink_preset_apply_by_name(name);
	if (ok) {
		/* Find preset to get its effect chain */
		struct vjlink_preset *preset = vjlink_preset_find(name);
		if (preset && preset->chain_length > 0) {
			strncpy(ctx->pending_effect,
			        preset->chain[0].effect_id,
			        sizeof(ctx->pending_effect) - 1);
			ctx->effect_pending = true;
		}
		blog(LOG_INFO,
		     "[VJLink] WebSocket: preset set to '%s'", name);
	}
	obs_data_set_bool(response_data, "success", ok);
}

static void handle_set_effect_direct(obs_data_t *request_data,
                                      obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();
	const char *effect_id = obs_data_get_string(request_data, "effect_id");

	if (!effect_id || !*effect_id) {
		/* Empty = clear effect */
		ctx->pending_effect[0] = '\0';
		ctx->effect_pending = true;
		ctx->active_preset_index = -1;
		vjlink_preset_set_index(-1);
		obs_data_set_bool(response_data, "success", true);
		blog(LOG_INFO, "[VJLink] WebSocket: effect cleared");
		return;
	}

	strncpy(ctx->pending_effect, effect_id,
	        sizeof(ctx->pending_effect) - 1);
	ctx->effect_pending = true;
	ctx->active_preset_index = -1;
	vjlink_preset_set_index(-1);
	obs_data_set_bool(response_data, "success", true);
	blog(LOG_INFO, "[VJLink] WebSocket: effect set to '%s'", effect_id);
}

static void handle_set_param(obs_data_t *request_data,
                              obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	const char *param = obs_data_get_string(request_data, "param");
	double value = obs_data_get_double(request_data, "value");

	if (param && *param) {
		/*
		 * Queue the param change for the render thread.
		 * Can't call gs_effect_set_* from the websocket thread.
		 */
		struct vjlink_context *ctx = vjlink_get_context();
		int idx = ctx->pending_param_count;
		if (idx < VJLINK_MAX_PENDING_PARAMS) {
			strncpy(ctx->pending_params[idx].name, param,
			        sizeof(ctx->pending_params[idx].name) - 1);
			ctx->pending_params[idx].name[
				sizeof(ctx->pending_params[idx].name) - 1] = '\0';
			ctx->pending_params[idx].value = (float)value;
			ctx->pending_param_count = idx + 1;
		}
		obs_data_set_bool(response_data, "success", true);
	} else {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error",
		                    "Missing param name");
	}
}

static void handle_next_preset(obs_data_t *request_data,
                                obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv);
	vjlink_preset_next();
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_int(response_data, "preset_index",
	                 vjlink_get_current_preset_index());
}

static void handle_prev_preset(obs_data_t *request_data,
                                obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv);
	vjlink_preset_prev();
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_int(response_data, "preset_index",
	                 vjlink_get_current_preset_index());
}

static void handle_tap_bpm(obs_data_t *request_data,
                             obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv);
	vjlink_tap_beat();

	struct vjlink_context *ctx = vjlink_get_context();
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_double(response_data, "bpm", (double)ctx->bpm);
}

static void handle_blackout(obs_data_t *request_data,
                              obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv);
	vjlink_toggle_blackout();
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_bool(response_data, "blackout", vjlink_is_blackout());
}

static void handle_get_state(obs_data_t *request_data,
                               obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();

	obs_data_set_bool(response_data, "success", true);
	obs_data_set_double(response_data, "bpm", (double)ctx->bpm);
	obs_data_set_double(response_data, "beat_phase",
	                    (double)ctx->beat_phase);
	obs_data_set_double(response_data, "beat_confidence",
	                    (double)ctx->beat_confidence);
	obs_data_set_double(response_data, "band_bass",
	                    (double)ctx->bands[0]);
	obs_data_set_double(response_data, "band_lowmid",
	                    (double)ctx->bands[1]);
	obs_data_set_double(response_data, "band_highmid",
	                    (double)ctx->bands[2]);
	obs_data_set_double(response_data, "band_treble",
	                    (double)ctx->bands[3]);
	obs_data_set_double(response_data, "rms", (double)ctx->rms);
	obs_data_set_double(response_data, "onset_strength",
	                    (double)ctx->onset_strength);

	/* Chronotensity (AudioLink-style cumulative energy per band) */
	obs_data_set_double(response_data, "chrono_bass",
	                    (double)ctx->chronotensity[0]);
	obs_data_set_double(response_data, "chrono_lowmid",
	                    (double)ctx->chronotensity[1]);
	obs_data_set_double(response_data, "chrono_highmid",
	                    (double)ctx->chronotensity[2]);
	obs_data_set_double(response_data, "chrono_treble",
	                    (double)ctx->chronotensity[3]);

	obs_data_set_int(response_data, "preset_index",
	                 vjlink_get_current_preset_index());
	obs_data_set_string(response_data, "effect_id",
	                    ctx->active_effect_id);
	obs_data_set_bool(response_data, "blackout", vjlink_is_blackout());

	/* Band sensitivity */
	obs_data_set_double(response_data, "sens_bass",
	                    (double)ctx->band_sensitivity[0]);
	obs_data_set_double(response_data, "sens_lowmid",
	                    (double)ctx->band_sensitivity[1]);
	obs_data_set_double(response_data, "sens_highmid",
	                    (double)ctx->band_sensitivity[2]);
	obs_data_set_double(response_data, "sens_treble",
	                    (double)ctx->band_sensitivity[3]);
	obs_data_set_double(response_data, "elapsed_time",
	                    (double)ctx->elapsed_time);

	/* LFO values */
	obs_data_t *lfo_data = obs_data_create();
	for (int i = 0; i < VJLINK_NUM_LFOS; i++) {
		char key[16];
		snprintf(key, sizeof(key), "lfo_%d", i);
		obs_data_set_double(lfo_data, key,
		                    (double)ctx->lfo_values[i]);
	}
	obs_data_set_obj(response_data, "lfos", lfo_data);
	obs_data_release(lfo_data);

	/* Band effect activation values */
	if (ctx->active_band_fx) {
		obs_data_t *band_fx_data = obs_data_create();
		for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
			char key[32];
			snprintf(key, sizeof(key), "band_%d_activation", i);
			obs_data_set_double(band_fx_data, key,
			     (double)ctx->active_band_fx->slots[i].current_activation);
			snprintf(key, sizeof(key), "band_%d_effect", i);
			obs_data_set_string(band_fx_data, key,
			     ctx->active_band_fx->slots[i].effect_id);
		}
		obs_data_set_obj(response_data, "band_effects", band_fx_data);
		obs_data_release(band_fx_data);
	}
}

/* --- Band Effect Handlers --- */

static void handle_set_band_effect(obs_data_t *request_data,
                                    obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();
	if (!ctx->active_band_fx) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "No active compositor");
		return;
	}

	int band = (int)obs_data_get_int(request_data, "band");
	const char *effect_id = obs_data_get_string(request_data, "effect_id");
	float threshold = (float)obs_data_get_double(request_data, "threshold");
	float intensity = (float)obs_data_get_double(request_data, "intensity");

	vjlink_band_effects_set_slot(ctx->active_band_fx, band,
	                              effect_id, threshold, intensity);

	obs_data_set_bool(response_data, "success", true);
	blog(LOG_INFO, "[VJLink] WebSocket: band %d effect set to '%s'",
	     band, effect_id);
}

static void handle_get_band_effects(obs_data_t *request_data,
                                     obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();
	obs_data_set_bool(response_data, "success", true);

	obs_data_array_t *arr = obs_data_array_create();

	if (ctx->active_band_fx) {
		for (int i = 0; i < VJLINK_NUM_BANDS; i++) {
			struct vjlink_band_slot *s = &ctx->active_band_fx->slots[i];
			obs_data_t *band_data = obs_data_create();
			obs_data_set_int(band_data, "band", i);
			obs_data_set_string(band_data, "effect_id", s->effect_id);
			obs_data_set_double(band_data, "threshold", (double)s->threshold);
			obs_data_set_double(band_data, "intensity", (double)s->intensity);
			obs_data_set_bool(band_data, "enabled", s->enabled);
			obs_data_set_double(band_data, "activation",
			                    (double)s->current_activation);
			obs_data_array_push_back(arr, band_data);
			obs_data_release(band_data);
		}
	}

	obs_data_set_array(response_data, "bands", arr);
	obs_data_array_release(arr);
}

static void handle_set_source_trigger(obs_data_t *request_data,
                                       obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();
	if (!ctx->active_source_triggers) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "No active compositor");
		return;
	}

	int slot = (int)obs_data_get_int(request_data, "slot");
	const char *source = obs_data_get_string(request_data, "source_name");
	int mode = (int)obs_data_get_int(request_data, "trigger_mode");
	int band = (int)obs_data_get_int(request_data, "trigger_band");
	float threshold = (float)obs_data_get_double(request_data, "threshold");
	float intensity = (float)obs_data_get_double(request_data, "intensity");
	const char *effect_id = obs_data_get_string(request_data, "effect_id");

	vjlink_source_trigger_set(ctx->active_source_triggers, slot,
	                           source, (enum vjlink_source_trigger_mode)mode,
	                           band, threshold, intensity);

	/* Set optional per-source effect */
	if (effect_id)
		vjlink_source_trigger_set_effect(ctx->active_source_triggers,
		                                  slot, effect_id);

	obs_data_set_bool(response_data, "success", true);
}

static void handle_get_source_triggers(obs_data_t *request_data,
                                        obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();
	obs_data_set_bool(response_data, "success", true);

	obs_data_array_t *arr = obs_data_array_create();

	if (ctx->active_source_triggers) {
		for (int i = 0; i < VJLINK_MAX_SOURCE_TRIGGERS; i++) {
			struct vjlink_source_trigger *t =
				&ctx->active_source_triggers->triggers[i];
			if (!t->enabled)
				continue;

			obs_data_t *td = obs_data_create();
			obs_data_set_int(td, "slot", i);
			obs_data_set_string(td, "source_name", t->source_name);
			obs_data_set_string(td, "effect_id", t->effect_id);
			obs_data_set_int(td, "trigger_mode", (int)t->trigger_mode);
			obs_data_set_int(td, "trigger_band", t->trigger_band);
			obs_data_set_double(td, "threshold", (double)t->threshold);
			obs_data_set_double(td, "intensity", (double)t->intensity);
			obs_data_set_bool(td, "currently_visible",
			                  t->currently_visible);
			obs_data_array_push_back(arr, td);
			obs_data_release(td);
		}
	}

	obs_data_set_array(response_data, "triggers", arr);
	obs_data_array_release(arr);
}

static void handle_set_media_layer(obs_data_t *request_data,
                                    obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();
	if (!ctx->active_media_layers) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "No active compositor");
		return;
	}

	int index = (int)obs_data_get_int(request_data, "layer");
	const char *path = obs_data_get_string(request_data, "path");
	int mode = (int)obs_data_get_int(request_data, "trigger_mode");
	int band = (int)obs_data_get_int(request_data, "trigger_band");
	float threshold = (float)obs_data_get_double(request_data, "threshold");
	float intensity = (float)obs_data_get_double(request_data, "intensity");

	vjlink_media_layer_set(ctx->active_media_layers, index, path,
	                        (enum vjlink_media_trigger_mode)mode,
	                        band, threshold, intensity);

	obs_data_set_bool(response_data, "success", true);
}

static void handle_get_media_layers(obs_data_t *request_data,
                                     obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();
	obs_data_set_bool(response_data, "success", true);

	obs_data_array_t *arr = obs_data_array_create();

	if (ctx->active_media_layers) {
		for (int i = 0; i < VJLINK_MAX_MEDIA_LAYERS; i++) {
			struct vjlink_media_layer *l =
				&ctx->active_media_layers->layers[i];
			if (!l->enabled)
				continue;

			obs_data_t *ld = obs_data_create();
			obs_data_set_int(ld, "layer", i);
			obs_data_set_string(ld, "path", l->path);
			obs_data_set_int(ld, "trigger_mode", (int)l->trigger_mode);
			obs_data_set_int(ld, "trigger_band", l->trigger_band);
			obs_data_set_double(ld, "threshold", (double)l->threshold);
			obs_data_set_double(ld, "intensity", (double)l->intensity);
			obs_data_set_double(ld, "current_opacity",
			                    (double)l->current_opacity);
			obs_data_array_push_back(arr, ld);
			obs_data_release(ld);
		}
	}

	obs_data_set_array(response_data, "layers", arr);
	obs_data_array_release(arr);
}

/* --- Scene/Source Discovery --- */

struct scene_enum_data {
	obs_data_array_t *scenes_arr;
};

static bool enum_scene_items_cb(obs_scene_t *scene, obs_sceneitem_t *item,
                                 void *param)
{
	UNUSED_PARAMETER(scene);
	obs_data_array_t *items_arr = param;

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;

	obs_data_t *item_data = obs_data_create();
	obs_data_set_string(item_data, "name", obs_source_get_name(src));
	obs_data_set_string(item_data, "type",
	                    obs_source_get_id(src));
	obs_data_set_bool(item_data, "visible",
	                  obs_sceneitem_visible(item));
	obs_data_array_push_back(items_arr, item_data);
	obs_data_release(item_data);

	return true;
}

/*
 * Use obs_enum_sources to find all sources. This is more reliable than
 * obs_enum_scenes which may not be available in all OBS versions.
 * We iterate all sources, identify scenes, then enumerate their items.
 */
static bool enum_all_sources_cb(void *data, obs_source_t *source)
{
	struct scene_enum_data *sed = data;

	/* Check if this source is a scene */
	obs_scene_t *scene = obs_scene_from_source(source);
	if (!scene)
		return true;

	obs_data_t *scene_data = obs_data_create();
	obs_data_set_string(scene_data, "name",
	                    obs_source_get_name(source));

	/* Enumerate items in this scene */
	obs_data_array_t *items_arr = obs_data_array_create();
	obs_scene_enum_items(scene, enum_scene_items_cb, items_arr);
	obs_data_set_array(scene_data, "sources", items_arr);
	obs_data_array_release(items_arr);

	obs_data_array_push_back(sed->scenes_arr, scene_data);
	obs_data_release(scene_data);

	return true;
}

static void handle_get_scene_sources(obs_data_t *request_data,
                                      obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv);

	obs_data_set_bool(response_data, "success", true);

	struct scene_enum_data sed;
	sed.scenes_arr = obs_data_array_create();

	/* Use obs_enum_sources (works in all OBS versions) */
	obs_enum_sources(enum_all_sources_cb, &sed);

	/* Also try obs_enum_scenes as backup if we found nothing */
	size_t scene_count = obs_data_array_count(sed.scenes_arr);
	if (scene_count == 0) {
		obs_enum_scenes(enum_all_sources_cb, &sed);
	}

	obs_data_set_array(response_data, "scenes", sed.scenes_arr);
	obs_data_array_release(sed.scenes_arr);

	blog(LOG_INFO, "[VJLink] GetSceneSources: found %u scenes",
	     (uint32_t)obs_data_array_count(
	         obs_data_get_array(response_data, "scenes")));
}

/* --- Sensitivity --- */

static void handle_set_sensitivity(obs_data_t *request_data,
                                    obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	struct vjlink_context *ctx = vjlink_get_context();
	int band = (int)obs_data_get_int(request_data, "band");
	double value = obs_data_get_double(request_data, "value");

	if (band >= 0 && band < 4 && value >= 0.0 && value <= 5.0) {
		ctx->band_sensitivity[band] = (float)value;
		obs_data_set_bool(response_data, "success", true);
	} else if (band == -1) {
		/* Set all bands at once */
		for (int i = 0; i < 4; i++) {
			char key[16];
			snprintf(key, sizeof(key), "band_%d", i);
			double v = obs_data_get_double(request_data, key);
			if (v > 0.0 && v <= 5.0)
				ctx->band_sensitivity[i] = (float)v;
		}
		obs_data_set_bool(response_data, "success", true);
	} else {
		obs_data_set_bool(response_data, "success", false);
	}
}

/* --- Band Effect Params --- */

static void handle_set_band_param(obs_data_t *request_data,
                                    obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);
	int band = (int)obs_data_get_int(request_data, "band");
	const char *param = obs_data_get_string(request_data, "param");
	double value = obs_data_get_double(request_data, "value");

	struct vjlink_context *ctx = vjlink_get_context();
	struct vjlink_band_effects *bfx = ctx->active_band_fx;

	if (!bfx || band < 0 || band >= VJLINK_NUM_BANDS || !param || !*param) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}

	struct vjlink_band_slot *slot = &bfx->slots[band];
	if (!slot->entry) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}

	for (uint32_t i = 0; i < slot->entry->param_count; i++) {
		if (strcmp(slot->entry->params[i].name, param) == 0) {
			slot->param_values[i][0] = (float)value;
			obs_data_set_bool(response_data, "success", true);
			blog(LOG_DEBUG, "[VJLink] SetBandParam band=%d '%s'=%.3f OK",
			     band, param, value);
			return;
		}
	}

	blog(LOG_DEBUG, "[VJLink] SetBandParam band=%d '%s' NO MATCH in '%s'",
	     band, param, slot->entry->id);
	obs_data_set_bool(response_data, "success", false);
}

/* --- Logo + Transparent BG --- */

static void handle_set_logo(obs_data_t *request_data,
                             obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);
	const char *path = obs_data_get_string(request_data, "path");
	int slot = (int)obs_data_get_int(request_data, "slot"); /* 0,1,2 */
	if (slot < 0 || slot > 2) slot = 0;
	struct vjlink_context *ctx = vjlink_get_context();

	if (path) {
		/* Strip surrounding quotes */
		size_t len = strlen(path);
		const char *start = path;
		if (len >= 2 && (path[0] == '"' || path[0] == '\'') &&
		    (path[len-1] == '"' || path[len-1] == '\'')) {
			start = path + 1;
			len -= 2;
		}
		char *target_path = (slot == 0) ? ctx->pending_logo_path :
		                    (slot == 1) ? ctx->pending_logo_path2 :
		                                  ctx->pending_logo_path3;
		volatile bool *target_flag = (slot == 0) ? &ctx->logo_pending :
		                             (slot == 1) ? &ctx->logo_pending2 :
		                                           &ctx->logo_pending3;
		size_t buf_size = sizeof(ctx->pending_logo_path);
		if (len >= buf_size) len = buf_size - 1;
		memcpy(target_path, start, len);
		target_path[len] = '\0';
		*target_flag = true;
		obs_data_set_bool(response_data, "success", true);
	} else {
		obs_data_set_bool(response_data, "success", false);
	}
}

static void handle_set_transparent_bg(obs_data_t *request_data,
                                       obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);
	bool enabled = obs_data_get_bool(request_data, "enabled");

	/* Set flag in context — compositor picks it up on next render */
	struct vjlink_context *ctx = vjlink_get_context();
	ctx->pending_transparent_bg = enabled;
	ctx->transparent_bg_pending = true;
	obs_data_set_bool(response_data, "success", true);
}

/* --- Init / Shutdown --- */

static void register_all_requests(void)
{
	vendor_request_register(g_vendor, "SetPreset",
	                        handle_set_preset, NULL);
	vendor_request_register(g_vendor, "SetParam",
	                        handle_set_param, NULL);
	vendor_request_register(g_vendor, "NextPreset",
	                        handle_next_preset, NULL);
	vendor_request_register(g_vendor, "PrevPreset",
	                        handle_prev_preset, NULL);
	vendor_request_register(g_vendor, "TapBPM",
	                        handle_tap_bpm, NULL);
	vendor_request_register(g_vendor, "Blackout",
	                        handle_blackout, NULL);
	vendor_request_register(g_vendor, "GetState",
	                        handle_get_state, NULL);
	vendor_request_register(g_vendor, "SetBandEffect",
	                        handle_set_band_effect, NULL);
	vendor_request_register(g_vendor, "GetBandEffects",
	                        handle_get_band_effects, NULL);
	vendor_request_register(g_vendor, "SetSourceTrigger",
	                        handle_set_source_trigger, NULL);
	vendor_request_register(g_vendor, "GetSourceTriggers",
	                        handle_get_source_triggers, NULL);
	vendor_request_register(g_vendor, "SetMediaLayer",
	                        handle_set_media_layer, NULL);
	vendor_request_register(g_vendor, "GetMediaLayers",
	                        handle_get_media_layers, NULL);
	vendor_request_register(g_vendor, "SetEffect",
	                        handle_set_effect_direct, NULL);
	vendor_request_register(g_vendor, "GetSceneSources",
	                        handle_get_scene_sources, NULL);
	vendor_request_register(g_vendor, "SetSensitivity",
	                        handle_set_sensitivity, NULL);
	vendor_request_register(g_vendor, "SetLogo",
	                        handle_set_logo, NULL);
	vendor_request_register(g_vendor, "SetTransparentBg",
	                        handle_set_transparent_bg, NULL);
	vendor_request_register(g_vendor, "SetBandParam",
	                        handle_set_band_param, NULL);
}

void vjlink_websocket_init(void)
{
	blog(LOG_INFO, "[VJLink] obs-websocket vendor API check");

	/* Try to register vendor - obs-websocket may or may not be loaded yet */
	g_vendor = vendor_register("obs-vjlink");

	if (!g_vendor) {
		blog(LOG_INFO,
		     "[VJLink] obs-websocket not yet loaded, "
		     "will retry after all modules are loaded");
		return;
	}

	register_all_requests();
	g_ws_available = true;

	blog(LOG_INFO,
	     "[VJLink] WebSocket vendor registered (18 request types)");
}

/* Called after all OBS modules have loaded (from plugin post-load event) */
void vjlink_websocket_late_init(void)
{
	if (g_ws_available)
		return; /* Already registered */

	g_vendor = vendor_register("obs-vjlink");
	if (!g_vendor) {
		blog(LOG_INFO,
		     "[VJLink] obs-websocket vendor API not available "
		     "(commands will work via hotkeys only)");
		return;
	}

	register_all_requests();
	g_ws_available = true;

	blog(LOG_INFO,
	     "[VJLink] WebSocket vendor registered late (18 request types)");
}

void vjlink_websocket_shutdown(void)
{
	if (!g_ws_available || !g_vendor)
		return;

	/*
	 * During OBS shutdown (obs_shutdown → free_module), obs-websocket
	 * may already be unloaded before our plugin. Calling
	 * vendor_request_unregister would access freed memory in the
	 * obs-websocket proc handler, causing an access violation crash.
	 *
	 * Since OBS is shutting down anyway, it cleans up all vendor
	 * registrations automatically. We just null our pointers.
	 */
	g_vendor = NULL;
	g_ws_ph = NULL;
	g_ws_available = false;
	blog(LOG_INFO, "[VJLink] WebSocket vendor shutdown (skipped unregister - OBS handles cleanup)");
}
