#include "source_trigger.h"
#include <obs-module.h>
#include <string.h>
#include <math.h>

void vjlink_source_triggers_init(struct vjlink_source_triggers *st)
{
	if (!st)
		return;

	memset(st, 0, sizeof(*st));

	for (int i = 0; i < VJLINK_MAX_SOURCE_TRIGGERS; i++) {
		st->triggers[i].threshold = 0.3f;
		st->triggers[i].intensity = 1.0f;
		st->triggers[i].currently_visible = true;
	}

	st->initialized = true;
	blog(LOG_INFO, "[VJLink] Source triggers initialized");
}

void vjlink_source_trigger_set(struct vjlink_source_triggers *st, int slot,
                                const char *source_name,
                                enum vjlink_source_trigger_mode mode,
                                int trigger_band, float threshold,
                                float intensity)
{
	if (!st || slot < 0 || slot >= VJLINK_MAX_SOURCE_TRIGGERS)
		return;

	struct vjlink_source_trigger *trig = &st->triggers[slot];

	if (!source_name || !source_name[0]) {
		vjlink_source_trigger_clear(st, slot);
		return;
	}

	strncpy(trig->source_name, source_name, sizeof(trig->source_name) - 1);
	trig->trigger_mode = mode;
	trig->trigger_band = (trigger_band >= 0 && trigger_band < VJLINK_NUM_BANDS)
		? trigger_band : 0;
	trig->threshold = fmaxf(0.0f, fminf(1.0f, threshold));
	trig->intensity = fmaxf(0.0f, fminf(2.0f, intensity));
	trig->enabled = true;
	trig->initial_synced = false;

	blog(LOG_INFO, "[VJLink] Source trigger %d: '%s' band=%d thresh=%.2f",
	     slot, source_name, trigger_band, threshold);
}

void vjlink_source_trigger_clear(struct vjlink_source_triggers *st, int slot)
{
	if (!st || slot < 0 || slot >= VJLINK_MAX_SOURCE_TRIGGERS)
		return;

	struct vjlink_source_trigger *trig = &st->triggers[slot];
	trig->source_name[0] = '\0';
	trig->effect_id[0] = '\0';
	trig->enabled = false;
}

void vjlink_source_trigger_set_effect(struct vjlink_source_triggers *st,
                                       int slot, const char *effect_id)
{
	if (!st || slot < 0 || slot >= VJLINK_MAX_SOURCE_TRIGGERS)
		return;

	struct vjlink_source_trigger *trig = &st->triggers[slot];
	if (effect_id && *effect_id) {
		strncpy(trig->effect_id, effect_id, sizeof(trig->effect_id) - 1);
		trig->effect_id[sizeof(trig->effect_id) - 1] = '\0';
	} else {
		trig->effect_id[0] = '\0';
	}
}

static bool should_trigger(struct vjlink_source_trigger *trig)
{
	struct vjlink_context *ctx = vjlink_get_context();

	switch (trig->trigger_mode) {
	case VJLINK_SRC_ALWAYS_ON:
		return true;

	case VJLINK_SRC_BAND_TRIGGER: {
		float band_val = ctx->bands[trig->trigger_band];
		return band_val > trig->threshold;
	}

	case VJLINK_SRC_BEAT_TRIGGER: {
		float envelope = 1.0f - ctx->beat_phase;
		return envelope > trig->threshold;
	}

	case VJLINK_SRC_GATE: {
		float band_val = ctx->bands[trig->trigger_band];
		return band_val > trig->threshold;
	}

	default:
		return true;
	}
}

struct find_source_data {
	const char *name;
	bool visible;
	bool found;
};

static bool find_in_group_callback(obs_scene_t *scene, obs_sceneitem_t *item,
                                    void *data)
{
	UNUSED_PARAMETER(scene);
	struct find_source_data *fsd = data;

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;

	const char *src_name = obs_source_get_name(src);
	if (src_name && strcmp(src_name, fsd->name) == 0) {
		obs_sceneitem_set_visible(item, fsd->visible);
		fsd->found = true;
		return false; /* stop */
	}

	/* Recurse into groups */
	if (obs_sceneitem_is_group(item)) {
		obs_scene_t *group_scene = obs_sceneitem_group_get_scene(item);
		if (group_scene)
			obs_scene_enum_items(group_scene,
			                      find_in_group_callback, data);
		if (fsd->found)
			return false;
	}

	return true;
}

static bool enum_scenes_callback(void *data, obs_source_t *source)
{
	struct find_source_data *fsd = data;
	obs_scene_t *scene = obs_scene_from_source(source);
	if (!scene)
		return true; /* continue */

	/* First try direct lookup */
	obs_sceneitem_t *item = obs_scene_find_source(scene, fsd->name);
	if (item) {
		obs_sceneitem_set_visible(item, fsd->visible);
		fsd->found = true;
		return true; /* check all scenes */
	}

	/* Recursive search through groups */
	obs_scene_enum_items(scene, find_in_group_callback, fsd);

	return true; /* check all scenes */
}

static void set_source_visible(const char *name, bool visible)
{
	struct find_source_data fsd = {
		.name = name,
		.visible = visible,
		.found = false,
	};
	obs_enum_scenes(enum_scenes_callback, &fsd);

	if (!fsd.found) {
		blog(LOG_DEBUG, "[VJLink] Source trigger: source '%s' "
		     "not found in any scene", name);
	}
}

void vjlink_source_triggers_update(struct vjlink_source_triggers *st)
{
	if (!st || !st->initialized)
		return;

	for (int i = 0; i < VJLINK_MAX_SOURCE_TRIGGERS; i++) {
		struct vjlink_source_trigger *trig = &st->triggers[i];
		if (!trig->enabled || !trig->source_name[0])
			continue;

		bool should_show = should_trigger(trig);

		/* Force initial sync: on first update after enabling,
		 * always set visibility regardless of current state */
		if (!trig->initial_synced) {
			trig->initial_synced = true;
			trig->currently_visible = !should_show; /* force change */
		}

		/* Only update if visibility changed */
		if (should_show != trig->currently_visible) {
			trig->currently_visible = should_show;
			set_source_visible(trig->source_name, should_show);
		}
	}
}
