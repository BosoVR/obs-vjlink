/*
 * Minimal OSC sender for VJLink.
 * Sends state broadcasts (BPM, bands, beat, onsets, palette) over UDP.
 * Default OFF — opt-in via WebSocket SetOscConfig.
 *
 * OSC packet format reference:
 *   <address>\0pad  <,types>\0pad  <args binary big-endian>
 */

#include "osc_sender.h"
#include "../vjlink_context.h"
#include <obs-module.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define VJ_SOCKET SOCKET
#define VJ_INVALID_SOCKET INVALID_SOCKET
#define VJ_SOCKET_ERROR SOCKET_ERROR
#define vj_close_sock closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define VJ_SOCKET int
#define VJ_INVALID_SOCKET -1
#define VJ_SOCKET_ERROR -1
#define vj_close_sock close
#endif

static struct {
	bool        enabled;
	char        host[64];
	int         port;
	int         rate_hz;
	VJ_SOCKET   sock;
	struct sockaddr_in addr;
	bool        addr_valid;
	bool        winsock_init;
	uint32_t    last_send_ms;
	float       elapsed_ms;
} g_osc = { 0 };

static uint32_t now_ms(void)
{
#ifdef _WIN32
	return (uint32_t)GetTickCount();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

bool vjlink_osc_init(void)
{
	if (g_osc.sock != 0 && g_osc.sock != VJ_INVALID_SOCKET)
		return true;

#ifdef _WIN32
	if (!g_osc.winsock_init) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			blog(LOG_WARNING, "[VJLink] OSC: WSAStartup failed");
			return false;
		}
		g_osc.winsock_init = true;
	}
#endif

	g_osc.sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (g_osc.sock == VJ_INVALID_SOCKET) {
		blog(LOG_WARNING, "[VJLink] OSC: socket() failed");
		return false;
	}

	/* Defaults if never configured */
	if (!g_osc.host[0]) strcpy(g_osc.host, "127.0.0.1");
	if (g_osc.port == 0) g_osc.port = 7000;
	if (g_osc.rate_hz == 0) g_osc.rate_hz = 30;

	return true;
}

void vjlink_osc_shutdown(void)
{
	if (g_osc.sock != 0 && g_osc.sock != VJ_INVALID_SOCKET) {
		vj_close_sock(g_osc.sock);
		g_osc.sock = VJ_INVALID_SOCKET;
	}
#ifdef _WIN32
	if (g_osc.winsock_init) {
		WSACleanup();
		g_osc.winsock_init = false;
	}
#endif
}

void vjlink_osc_configure(bool enabled, const char *host, int port, int rate_hz)
{
	g_osc.enabled = enabled;
	if (host && host[0]) {
		strncpy(g_osc.host, host, sizeof(g_osc.host) - 1);
		g_osc.host[sizeof(g_osc.host) - 1] = '\0';
	}
	if (port >= 1024 && port <= 65535) g_osc.port = port;
	if (rate_hz >= 1 && rate_hz <= 120) g_osc.rate_hz = rate_hz;

	/* Resolve address */
	memset(&g_osc.addr, 0, sizeof(g_osc.addr));
	g_osc.addr.sin_family = AF_INET;
	g_osc.addr.sin_port = htons((u_short)g_osc.port);
	if (inet_pton(AF_INET, g_osc.host, &g_osc.addr.sin_addr) == 1) {
		g_osc.addr_valid = true;
	} else {
		g_osc.addr_valid = false;
	}

	if (enabled && !g_osc.sock) vjlink_osc_init();

	blog(LOG_INFO, "[VJLink] OSC: %s -> %s:%d @ %d Hz",
	     enabled ? "ON" : "OFF", g_osc.host, g_osc.port, g_osc.rate_hz);
}

void vjlink_osc_get_config(bool *enabled, char *host_out, int host_max,
                           int *port, int *rate_hz)
{
	if (enabled) *enabled = g_osc.enabled;
	if (host_out && host_max > 0) {
		strncpy(host_out, g_osc.host, host_max - 1);
		host_out[host_max - 1] = '\0';
	}
	if (port) *port = g_osc.port;
	if (rate_hz) *rate_hz = g_osc.rate_hz;
}

/* === OSC packet builder ============================================== */

/* Pad to 4 bytes */
static int pad4(int n) { return (n + 3) & ~3; }

/* Append big-endian float */
static int osc_append_float(char *buf, int pos, float v)
{
	uint32_t u;
	memcpy(&u, &v, 4);
	buf[pos+0] = (char)((u >> 24) & 0xFF);
	buf[pos+1] = (char)((u >> 16) & 0xFF);
	buf[pos+2] = (char)((u >>  8) & 0xFF);
	buf[pos+3] = (char)( u        & 0xFF);
	return pos + 4;
}

static int osc_append_int(char *buf, int pos, int32_t v)
{
	uint32_t u = (uint32_t)v;
	buf[pos+0] = (char)((u >> 24) & 0xFF);
	buf[pos+1] = (char)((u >> 16) & 0xFF);
	buf[pos+2] = (char)((u >>  8) & 0xFF);
	buf[pos+3] = (char)( u        & 0xFF);
	return pos + 4;
}

static int osc_append_string(char *buf, int pos, const char *s)
{
	int len = (int)strlen(s);
	memcpy(buf + pos, s, len);
	buf[pos + len] = '\0';
	int total = pad4(len + 1);
	for (int i = len + 1; i < total; i++) buf[pos + i] = '\0';
	return pos + total;
}

/* Build a single OSC message. Returns total bytes. */
static int osc_build(char *buf, const char *addr, const char *types,
                     const float *fargs, int nfloat,
                     const int *iargs, int nint)
{
	int pos = 0;
	pos = osc_append_string(buf, pos, addr);
	/* Type tag string starts with comma */
	char tag[32];
	tag[0] = ',';
	int tl = 1;
	for (int i = 0; types[i] && tl < 30; i++) tag[tl++] = types[i];
	tag[tl] = '\0';
	pos = osc_append_string(buf, pos, tag);

	int fi = 0, ii = 0;
	for (int i = 0; types[i]; i++) {
		if (types[i] == 'f') pos = osc_append_float(buf, pos, fargs[fi++]);
		else if (types[i] == 'i') pos = osc_append_int(buf, pos, iargs[ii++]);
	}
	return pos;
}

static void osc_send(const char *buf, int len)
{
	if (!g_osc.addr_valid || g_osc.sock == 0 || g_osc.sock == VJ_INVALID_SOCKET) return;
	sendto(g_osc.sock, buf, len, 0,
	       (struct sockaddr *)&g_osc.addr, sizeof(g_osc.addr));
}

void vjlink_osc_tick(void)
{
	if (!g_osc.enabled) return;
	if (!g_osc.sock) {
		if (!vjlink_osc_init()) return;
	}
	if (!g_osc.addr_valid) {
		vjlink_osc_configure(g_osc.enabled, g_osc.host, g_osc.port, g_osc.rate_hz);
		if (!g_osc.addr_valid) return;
	}

	uint32_t now = now_ms();
	uint32_t interval = (g_osc.rate_hz > 0) ? (1000 / g_osc.rate_hz) : 33;
	if (now - g_osc.last_send_ms < interval) return;
	g_osc.last_send_ms = now;

	struct vjlink_context *ctx = vjlink_get_context();
	char buf[256];
	int n;

	/* /vjlink/bpm f */
	float fargs1[1] = { ctx->bpm };
	n = osc_build(buf, "/vjlink/bpm", "f", fargs1, 1, NULL, 0);
	osc_send(buf, n);

	/* /vjlink/beat ffff = phase, 1/4, 1/8, 1/16 */
	float fargs2[4] = { ctx->beat_phase, ctx->beat_1_4, ctx->beat_1_8, ctx->beat_1_16 };
	n = osc_build(buf, "/vjlink/beat", "ffff", fargs2, 4, NULL, 0);
	osc_send(buf, n);

	/* /vjlink/bands ffff */
	float fargs3[4] = { ctx->bands[0], ctx->bands[1], ctx->bands[2], ctx->bands[3] };
	n = osc_build(buf, "/vjlink/bands", "ffff", fargs3, 4, NULL, 0);
	osc_send(buf, n);

	/* /vjlink/onsets ffff = onset, kick, snare, hat */
	float fargs4[4] = { ctx->onset_strength, ctx->kick_onset,
	                    ctx->snare_onset, ctx->hat_onset };
	n = osc_build(buf, "/vjlink/onsets", "ffff", fargs4, 4, NULL, 0);
	osc_send(buf, n);

	/* /vjlink/palette i */
	int iargs1[1] = { ctx->palette_id };
	n = osc_build(buf, "/vjlink/palette", "i", NULL, 0, iargs1, 1);
	osc_send(buf, n);

	/* /vjlink/macros ffff */
	float fargs5[4] = { ctx->macro_energy, ctx->macro_chaos,
	                    ctx->macro_speed, ctx->macro_color };
	n = osc_build(buf, "/vjlink/macros", "ffff", fargs5, 4, NULL, 0);
	osc_send(buf, n);
}
