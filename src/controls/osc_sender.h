#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize OSC sender (WinSock + buffers). Idempotent. */
bool vjlink_osc_init(void);

/* Shutdown — closes socket and frees buffers. */
void vjlink_osc_shutdown(void);

/* Configure / enable. host = "127.0.0.1" (or LAN IP), port 1024-65535,
 * rate_hz = how often to broadcast state (5..60). */
void vjlink_osc_configure(bool enabled, const char *host, int port, int rate_hz);

/* Get current config — for GetState reply */
void vjlink_osc_get_config(bool *enabled, char *host_out, int host_max,
                           int *port, int *rate_hz);

/* Tick from render thread. Internal rate-limit by rate_hz. */
void vjlink_osc_tick(void);

#ifdef __cplusplus
}
#endif
