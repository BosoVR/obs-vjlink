#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VJLink WebSocket Handler
 *
 * Registers custom vendor events with obs-websocket (v5+).
 * External controllers can send commands via:
 *
 *   VJLink.SetPreset    { "preset_name": "Neon Tunnel" }
 *   VJLink.SetParam     { "param": "tunnel_speed", "value": 2.0 }
 *   VJLink.NextPreset   {}
 *   VJLink.PrevPreset   {}
 *   VJLink.TapBPM       {}
 *   VJLink.Blackout     {}
 *   VJLink.GetState     {} -> returns current state
 *
 * Requires obs-websocket plugin (bundled with OBS 28+).
 */

/* Initialize vendor event registration (call from obs_module_load) */
void vjlink_websocket_init(void);

/* Retry registration after all modules loaded */
void vjlink_websocket_late_init(void);

/* Cleanup (call from obs_module_unload) */
void vjlink_websocket_shutdown(void);

#ifdef __cplusplus
}
#endif
