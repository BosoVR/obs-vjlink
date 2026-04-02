#pragma once

#include "rendering/effect_system.h"
#include <obs-module.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Properties Builder
 *
 * Generates OBS property controls from effect metadata JSON files.
 * Creates grouped, labeled controls with proper ranges and tooltips.
 */

/* Add effect selection dropdown grouped by category */
void vjlink_props_add_effect_list(obs_properties_t *props,
                                   const char *prop_name,
                                   const char *label);

/* Add dynamic parameters for the currently selected effect */
void vjlink_props_add_effect_params(obs_properties_t *props,
                                     const char *effect_id);

/* Build preset selection dropdown from loaded presets */
void vjlink_props_add_preset_list(obs_properties_t *props,
                                   const char *prop_name,
                                   const char *label);

#ifdef __cplusplus
}
#endif
