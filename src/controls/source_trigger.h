#pragma once

#include "vjlink_context.h"
#include <obs-module.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VJLINK_MAX_SOURCE_TRIGGERS 8

/* Trigger modes (same as media layer) */
enum vjlink_source_trigger_mode {
	VJLINK_SRC_ALWAYS_ON = 0,
	VJLINK_SRC_BAND_TRIGGER,
	VJLINK_SRC_BEAT_TRIGGER,
	VJLINK_SRC_GATE,
};

/* A source trigger slot: controls visibility of an OBS source */
struct vjlink_source_trigger {
	char             source_name[256];
	char             effect_id[64];   /* optional effect to apply */
	bool             enabled;
	int              trigger_band;     /* 0-3 */
	float            threshold;
	float            intensity;
	enum vjlink_source_trigger_mode trigger_mode;
	bool             currently_visible;
	bool             initial_synced;
};

/* Source trigger system */
struct vjlink_source_triggers {
	struct vjlink_source_trigger triggers[VJLINK_MAX_SOURCE_TRIGGERS];
	bool initialized;
};

/* Initialize source trigger system */
void vjlink_source_triggers_init(struct vjlink_source_triggers *st);

/* Set a source trigger */
void vjlink_source_trigger_set(struct vjlink_source_triggers *st, int slot,
                                const char *source_name,
                                enum vjlink_source_trigger_mode mode,
                                int trigger_band, float threshold,
                                float intensity);

/* Set effect for a source trigger slot */
void vjlink_source_trigger_set_effect(struct vjlink_source_triggers *st,
                                       int slot, const char *effect_id);

/* Clear a source trigger */
void vjlink_source_trigger_clear(struct vjlink_source_triggers *st, int slot);

/* Update all triggers (call per frame - evaluates audio and toggles sources) */
void vjlink_source_triggers_update(struct vjlink_source_triggers *st);

#ifdef __cplusplus
}
#endif
