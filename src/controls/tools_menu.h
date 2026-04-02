#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * VJLink Tools Menu
 *
 * Adds "VJLink Settings" entry to OBS Tools menu.
 * Settings dialog for HTTP server (port, auto-start, start/stop).
 */

/* Initialize tools menu (call from obs_module_load) */
void vjlink_tools_menu_init(void);

/* Load saved settings and optionally auto-start HTTP server */
void vjlink_tools_apply_saved_settings(void);

/* Get saved HTTP settings */
uint16_t vjlink_tools_get_http_port(void);
bool vjlink_tools_get_http_autostart(void);

/* Cleanup */
void vjlink_tools_menu_shutdown(void);
