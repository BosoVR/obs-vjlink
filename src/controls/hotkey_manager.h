#pragma once

#include <obs-module.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VJLink Hotkey Manager
 *
 * Registers OBS frontend hotkeys for live VJ control:
 *   - Next/Previous preset
 *   - Specific preset slots (1-10)
 *   - Beat tap (manual BPM)
 *   - Toggle effect on/off
 *   - Blackout (fade to black)
 */

/* Initialize hotkey registrations (call from obs_module_load) */
void vjlink_hotkeys_init(void);

/* Cleanup (call from obs_module_unload) */
void vjlink_hotkeys_shutdown(void);

/* Manual beat tap (can be called externally) */
void vjlink_tap_beat(void);

/* Navigate presets */
void vjlink_preset_next(void);
void vjlink_preset_prev(void);

/* Set preset by index */
void vjlink_preset_set_index(int index);

/* Toggle blackout */
void vjlink_toggle_blackout(void);

/* Query state */
bool vjlink_is_blackout(void);
int  vjlink_get_current_preset_index(void);

#ifdef __cplusplus
}
#endif
