#include "http_server.h"
#include <obs-module.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define closesocket_fn closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
typedef int socket_t;
#define INVALID_SOCK -1
#define closesocket_fn close
#endif

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Mini HTTP server - serves static files from the plugin data directory.
 * Single-threaded with select() for simplicity. Handles one request at a time.
 */

static volatile bool g_running = false;
static socket_t g_listen_sock = INVALID_SOCK;
static pthread_t g_server_thread;
static uint16_t g_port = 0;
static char g_webui_path[512] = {0};

/* MIME type lookup */
static const char *get_mime_type(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return "application/octet-stream";
	if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
		return "text/html; charset=utf-8";
	if (strcmp(ext, ".css") == 0)
		return "text/css; charset=utf-8";
	if (strcmp(ext, ".js") == 0)
		return "application/javascript; charset=utf-8";
	if (strcmp(ext, ".json") == 0)
		return "application/json; charset=utf-8";
	if (strcmp(ext, ".png") == 0)
		return "image/png";
	if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
		return "image/jpeg";
	if (strcmp(ext, ".gif") == 0)
		return "image/gif";
	if (strcmp(ext, ".svg") == 0)
		return "image/svg+xml";
	if (strcmp(ext, ".ico") == 0)
		return "image/x-icon";
	if (strcmp(ext, ".woff2") == 0)
		return "font/woff2";
	return "application/octet-stream";
}

/* Send HTTP response */
static void send_response(socket_t client, int status, const char *status_text,
                           const char *content_type, const char *body,
                           size_t body_len)
{
	char header[512];
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Connection: close\r\n"
		"\r\n",
		status, status_text, content_type, body_len);

	send(client, header, hlen, 0);
	if (body && body_len > 0) {
		size_t sent = 0;
		while (sent < body_len) {
			int chunk = (int)(body_len - sent);
			if (chunk > 8192) chunk = 8192;
			int n = send(client, body + sent, chunk, 0);
			if (n <= 0) break;
			sent += n;
		}
	}
}

/* Send 404 */
static void send_404(socket_t client)
{
	const char *body = "<h1>404 Not Found</h1>";
	send_response(client, 404, "Not Found", "text/html", body,
	              strlen(body));
}

/* Read file and send as response */
static void serve_file(socket_t client, const char *filepath)
{
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		send_404(client);
		return;
	}

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (fsize <= 0 || fsize > 10 * 1024 * 1024) { /* max 10MB */
		fclose(f);
		send_404(client);
		return;
	}

	char *data = malloc(fsize);
	if (!data) {
		fclose(f);
		send_404(client);
		return;
	}

	size_t read = fread(data, 1, fsize, f);
	fclose(f);

	const char *mime = get_mime_type(filepath);
	send_response(client, 200, "OK", mime, data, read);
	free(data);
}

/* Parse HTTP request and serve file */
static void handle_client(socket_t client)
{
	char buf[4096];
	int received = recv(client, buf, sizeof(buf) - 1, 0);
	if (received <= 0) return;
	buf[received] = '\0';

	/* Parse method and path from first line: "GET /path HTTP/1.1" */
	if (strncmp(buf, "GET ", 4) != 0) {
		const char *body = "<h1>405 Method Not Allowed</h1>";
		send_response(client, 405, "Method Not Allowed", "text/html",
		              body, strlen(body));
		return;
	}

	char *path_start = buf + 4;
	char *path_end = strchr(path_start, ' ');
	if (!path_end) return;
	*path_end = '\0';

	/* Security: reject paths with ".." */
	if (strstr(path_start, "..")) {
		const char *body = "<h1>403 Forbidden</h1>";
		send_response(client, 403, "Forbidden", "text/html",
		              body, strlen(body));
		return;
	}

	/* Default: serve vjlink-control.html */
	const char *request_path = path_start;
	if (strcmp(request_path, "/") == 0)
		request_path = "/vjlink-control.html";

	/* Strip query string */
	char clean_path[256];
	strncpy(clean_path, request_path, sizeof(clean_path) - 1);
	clean_path[sizeof(clean_path) - 1] = '\0';
	char *query = strchr(clean_path, '?');
	if (query) *query = '\0';

	/* Build full filesystem path */
	char filepath[1024];
	snprintf(filepath, sizeof(filepath), "%s%s", g_webui_path, clean_path);

	/* Convert forward slashes to backslashes on Windows */
#ifdef _WIN32
	for (char *p = filepath; *p; p++) {
		if (*p == '/') *p = '\\';
	}
#endif

	serve_file(client, filepath);
}

/* Server thread */
static void *server_thread_func(void *arg)
{
	UNUSED_PARAMETER(arg);

	blog(LOG_INFO, "[VJLink] HTTP server running on port %u", g_port);

	while (g_running) {
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(g_listen_sock, &readfds);

		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 200000; /* 200ms timeout for clean shutdown */

		int ready = select((int)g_listen_sock + 1, &readfds, NULL,
		                   NULL, &tv);
		if (ready <= 0) continue;

		if (FD_ISSET(g_listen_sock, &readfds)) {
			struct sockaddr_in client_addr;
			int addr_len = sizeof(client_addr);
			socket_t client = accept(g_listen_sock,
				(struct sockaddr *)&client_addr, &addr_len);

			if (client != INVALID_SOCK) {
				handle_client(client);
				closesocket_fn(client);
			}
		}
	}

	blog(LOG_INFO, "[VJLink] HTTP server thread exiting");
	return NULL;
}

bool vjlink_http_server_start(uint16_t port, const char *webui_dir)
{
	if (g_running) return true;

	if (!webui_dir || !*webui_dir) {
		blog(LOG_ERROR, "[VJLink] No web-ui directory provided");
		return false;
	}

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		blog(LOG_ERROR, "[VJLink] WSAStartup failed");
		return false;
	}
#endif

	strncpy(g_webui_path, webui_dir, sizeof(g_webui_path) - 1);
	g_webui_path[sizeof(g_webui_path) - 1] = '\0';

	blog(LOG_INFO, "[VJLink] HTTP server serving from: %s", g_webui_path);

	/* Create listening socket */
	g_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_listen_sock == INVALID_SOCK) {
		blog(LOG_ERROR, "[VJLink] Failed to create HTTP socket");
		return false;
	}

	/* Allow address reuse */
	int opt = 1;
	setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR,
	           (const char *)&opt, sizeof(opt));

	/* Bind to port */
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* localhost only */
	addr.sin_port = htons(port);

	if (bind(g_listen_sock, (struct sockaddr *)&addr,
	         sizeof(addr)) != 0) {
		blog(LOG_ERROR, "[VJLink] Failed to bind HTTP server to port %u "
		     "(port in use?)", port);
		closesocket_fn(g_listen_sock);
		g_listen_sock = INVALID_SOCK;
		return false;
	}

	if (listen(g_listen_sock, 5) != 0) {
		blog(LOG_ERROR, "[VJLink] Failed to listen on HTTP socket");
		closesocket_fn(g_listen_sock);
		g_listen_sock = INVALID_SOCK;
		return false;
	}

	g_port = port;
	g_running = true;

	/* Start server thread */
	if (pthread_create(&g_server_thread, NULL, server_thread_func,
	                   NULL) != 0) {
		blog(LOG_ERROR, "[VJLink] Failed to create HTTP server thread");
		g_running = false;
		closesocket_fn(g_listen_sock);
		g_listen_sock = INVALID_SOCK;
		return false;
	}

	blog(LOG_INFO,
	     "[VJLink] HTTP server started: http://localhost:%u", port);
	return true;
}

void vjlink_http_server_stop(void)
{
	if (!g_running) return;

	g_running = false;

	/* Close listening socket to unblock select() */
	if (g_listen_sock != INVALID_SOCK) {
		closesocket_fn(g_listen_sock);
		g_listen_sock = INVALID_SOCK;
	}

	/* Wait for thread to finish */
	pthread_join(g_server_thread, NULL);

#ifdef _WIN32
	WSACleanup();
#endif

	blog(LOG_INFO, "[VJLink] HTTP server stopped");
}

bool vjlink_http_server_is_running(void)
{
	return g_running;
}

uint16_t vjlink_http_server_get_port(void)
{
	return g_port;
}
