#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * VJLink Mini HTTP Server
 *
 * Serves the web-ui control panel on http://localhost:8088
 * Starts automatically with the OBS plugin.
 */

#define VJLINK_HTTP_DEFAULT_PORT 8088

bool vjlink_http_server_start(uint16_t port, const char *webui_dir);
void vjlink_http_server_stop(void);
bool vjlink_http_server_is_running(void);
uint16_t vjlink_http_server_get_port(void);
