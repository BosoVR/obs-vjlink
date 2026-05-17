#include "tools_menu.h"
#include "http_server.h"
#include <obs-module.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

/*
 * VJLink Tools Menu - Settings & About Dialog
 *
 * Dynamically loads obs-frontend-api to add a Tools menu entry.
 * Shows a Win32 dialog for HTTP server configuration + About button.
 * Saves settings to vjlink_config.ini in the plugin config directory.
 */

/* --- Config --- */

#define CONFIG_FILENAME "vjlink_config.ini"

static uint16_t g_http_port = VJLINK_HTTP_DEFAULT_PORT;
static bool g_http_autostart = true;

static char g_config_path[512] = {0};

static void build_config_path(void)
{
	if (g_config_path[0])
		return;

	char *path = obs_module_config_path(CONFIG_FILENAME);
	if (path) {
		strncpy(g_config_path, path, sizeof(g_config_path) - 1);
		bfree(path);
	}
}

static void save_config(void)
{
	build_config_path();
	if (!g_config_path[0])
		return;

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	FILE *f = fopen(g_config_path, "w");
	if (!f) {
		blog(LOG_WARNING, "[VJLink] Could not save config to %s",
		     g_config_path);
		return;
	}

	fprintf(f, "[http_server]\n");
	fprintf(f, "port=%u\n", (unsigned)g_http_port);
	fprintf(f, "autostart=%d\n", g_http_autostart ? 1 : 0);
	fclose(f);

	blog(LOG_INFO, "[VJLink] Config saved: port=%u, autostart=%d",
	     (unsigned)g_http_port, g_http_autostart);
}

static void load_config(void)
{
	build_config_path();
	if (!g_config_path[0])
		return;

	FILE *f = fopen(g_config_path, "r");
	if (!f) {
		blog(LOG_INFO,
		     "[VJLink] No config file, using defaults "
		     "(port=%u, autostart=%d)",
		     (unsigned)g_http_port, g_http_autostart);
		return;
	}

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		unsigned val;
		if (sscanf(line, "port=%u", &val) == 1) {
			if (val > 0 && val < 65536)
				g_http_port = (uint16_t)val;
		}
		int bval;
		if (sscanf(line, "autostart=%d", &bval) == 1) {
			g_http_autostart = (bval != 0);
		}
	}
	fclose(f);

	blog(LOG_INFO, "[VJLink] Config loaded: port=%u, autostart=%d",
	     (unsigned)g_http_port, g_http_autostart);
}

/* --- Win32 Dialog --- */

#ifdef _WIN32

/* Dialog control IDs */
#define IDC_PORT_EDIT    101
#define IDC_AUTOSTART    102
#define IDC_START_BTN    103
#define IDC_STOP_BTN     104
#define IDC_OPEN_UI_BTN  105
#define IDC_STATUS_LABEL 106
#define IDC_SAVE_BTN     107
#define IDC_ABOUT_BTN    108

/* About text */
static const char *g_about_text =
	"OBS-VJLink v1.4.0\r\n"
	"Audio-reactive VJ system for OBS Studio\r\n"
	"\r\n"
	"Built for live VJ sets, hardtechno/industrial LED walls,\r\n"
	"VRChat/Shelternet video-wall workflows and simple\r\n"
	"AudioLink-style operation.\r\n"
	"\r\n"
	"=== CORE WORKFLOW ===\r\n"
	"\r\n"
	"1. Add 'VJLink Audio Analyzer' to your music source.\r\n"
	"2. Add 'VJLink Compositor' to the scene.\r\n"
	"3. Open Tools > VJLink Settings > Open Web UI.\r\n"
	"4. Use Audio Analysis and Per-Band Effects:\r\n"
	"   Bass / Low-Mid / High-Mid / Treble.\r\n"
	"5. Threshold + Intensity stays simple:\r\n"
	"   band level over threshold -> effect triggers.\r\n"
	"\r\n"
	"Default Web UI: http://localhost:8088\r\n"
	"Default obs-websocket: ws://localhost:4455\r\n"
	"\r\n"
	"=== FEATURES ===\r\n"
	"\r\n"
	"AudioLink-style analysis:\r\n"
	"  Bass, Low-Mid, High-Mid, Treble, RMS, onsets,\r\n"
	"  chronotensity, BPM, beat phase and subdivisions.\r\n"
	"\r\n"
	"Basic + Pro Web UI:\r\n"
	"  Basic keeps the central Audio Analysis workflow.\r\n"
	"  Pro adds layer order, response shaping, macros,\r\n"
	"  palettes, pads, media routing and diagnostics.\r\n"
	"\r\n"
	"Effect engine:\r\n"
	"  Main generators, source filters, post FX, flash/strobe,\r\n"
	"  glitch, LED-wall, tunnel, geometric, retro, particles,\r\n"
	"  audio visualizers and hardtechno-oriented effects.\r\n"
	"\r\n"
	"Source and media control:\r\n"
	"  OBS source triggers by audio band, media layers,\r\n"
	"  source-filter automation, compositor/filter modes,\r\n"
	"  video wall support and transparent-background output.\r\n"
	"\r\n"
	"Logo and text systems:\r\n"
	"  3 logo slots, logo pulse/strobe/grid/runner effects,\r\n"
	"  custom text type animator and band-reactive overlays.\r\n"
	"\r\n"
	"Performance controls:\r\n"
	"  Pad banks A/B/C, favorites, snapshots, Auto-VJ,\r\n"
	"  VJ AI, STOP ALL AUTO, blackout, tap tempo and hotkeys.\r\n"
	"\r\n"
	"Safety and stage use:\r\n"
	"  Strobe safety limiter, transparent output, LED-wall\r\n"
	"  layouts, hard cuts, impact flashes and BPM-sync motion.\r\n"
	"\r\n"
	"=== ADVANCED SETUP ===\r\n"
	"\r\n"
	"- Use VJLink Effect as a filter on images, videos,\r\n"
	"  cameras, browser sources or VRChat captures.\r\n"
	"- Use Media & Source Triggers for band-gated show/hide.\r\n"
	"- Use Video Wall for multi-screen and LED panel looks.\r\n"
	"- Save Profiles / Packs for event-specific setups.\r\n"
	"\r\n"
	"=== NOTES ===\r\n"
	"\r\n"
	"License: free for personal/event VJ use. Commercial\r\n"
	"resale inside sold software requires permission.\r\n"
	"Discord contact: bosovr\r\n"
	"\r\n"
	"https://github.com/BosoVR/obs-vjlink\r\n";

static void update_status_label(HWND dlg)
{
	char status[128];
	if (vjlink_http_server_is_running()) {
		snprintf(status, sizeof(status),
		         "Status: Running on port %u",
		         (unsigned)vjlink_http_server_get_port());
	} else {
		snprintf(status, sizeof(status), "Status: Stopped");
	}
	SetDlgItemTextA(dlg, IDC_STATUS_LABEL, status);

	EnableWindow(GetDlgItem(dlg, IDC_START_BTN),
	             !vjlink_http_server_is_running());
	EnableWindow(GetDlgItem(dlg, IDC_STOP_BTN),
	             vjlink_http_server_is_running());
	EnableWindow(GetDlgItem(dlg, IDC_OPEN_UI_BTN),
	             vjlink_http_server_is_running());
}

static void show_about_dialog(HWND parent)
{
	/* Create a simple about dialog with a scrollable text box */
	BYTE buf[2048];
	memset(buf, 0, sizeof(buf));

	DLGTEMPLATE *dlg = (DLGTEMPLATE *)buf;
	dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION |
	             WS_SYSMENU | DS_SETFONT;
	dlg->cdit = 1;
	dlg->cx = 280;
	dlg->cy = 300;

	LPWORD ptr = (LPWORD)(dlg + 1);
	*ptr++ = 0; /* menu */
	*ptr++ = 0; /* class */

	LPCWSTR title = L"VJLink - About";
	int tlen = (int)wcslen(title) + 1;
	memcpy(ptr, title, tlen * sizeof(WCHAR));
	ptr += tlen;
	*ptr++ = 9; /* font size */
	LPCWSTR font = L"Segoe UI";
	int flen = (int)wcslen(font) + 1;
	memcpy(ptr, font, flen * sizeof(WCHAR));
	ptr += flen;

	/* Multiline read-only edit for about text */
	ULONG_PTR ul = (ULONG_PTR)ptr;
	ul = (ul + 3) & ~3;
	ptr = (LPWORD)ul;

	DLGITEMTEMPLATE *item = (DLGITEMTEMPLATE *)ptr;
	item->style = WS_CHILD | WS_VISIBLE | ES_MULTILINE |
	              ES_READONLY | WS_VSCROLL | WS_BORDER |
	              ES_AUTOVSCROLL;
	item->x = 6;
	item->y = 6;
	item->cx = 268;
	item->cy = 288;
	item->id = 200;
	ptr = (LPWORD)(item + 1);
	*ptr++ = 0xFFFF;
	*ptr++ = 0x0081; /* Edit */
	*ptr++ = 0;      /* no text */
	*ptr++ = 0;

	/* Use a simple lambda-style proc */
	DialogBoxIndirectParamA(NULL, dlg, parent,
	                        (DLGPROC)DefDlgProcA, 0);
}

/* About dialog proc */
static INT_PTR CALLBACK about_dlg_proc(HWND dlg, UINT msg,
                                        WPARAM wp, LPARAM lp)
{
	UNUSED_PARAMETER(lp);
	switch (msg) {
	case WM_INITDIALOG:
		SetDlgItemTextA(dlg, 200, g_about_text);
		return TRUE;
	case WM_COMMAND:
		if (LOWORD(wp) == IDCANCEL) {
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	case WM_CLOSE:
		EndDialog(dlg, 0);
		return TRUE;
	}
	return FALSE;
}

static void show_about(HWND parent)
{
	BYTE buf[2048];
	memset(buf, 0, sizeof(buf));

	DLGTEMPLATE *dlg = (DLGTEMPLATE *)buf;
	dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION |
	             WS_SYSMENU | DS_SETFONT;
	dlg->cdit = 1;
	dlg->cx = 280;
	dlg->cy = 300;

	LPWORD ptr = (LPWORD)(dlg + 1);
	*ptr++ = 0;
	*ptr++ = 0;

	LPCWSTR title = L"VJLink - About";
	int tlen = (int)wcslen(title) + 1;
	memcpy(ptr, title, tlen * sizeof(WCHAR));
	ptr += tlen;
	*ptr++ = 9;
	LPCWSTR font_name = L"Segoe UI";
	int flen = (int)wcslen(font_name) + 1;
	memcpy(ptr, font_name, flen * sizeof(WCHAR));
	ptr += flen;

	/* Multiline read-only edit */
	ULONG_PTR ul = (ULONG_PTR)ptr;
	ul = (ul + 3) & ~3;
	ptr = (LPWORD)ul;

	DLGITEMTEMPLATE *item = (DLGITEMTEMPLATE *)ptr;
	item->style = WS_CHILD | WS_VISIBLE | ES_MULTILINE |
	              ES_READONLY | WS_VSCROLL | WS_BORDER |
	              ES_AUTOVSCROLL;
	item->x = 6;
	item->y = 6;
	item->cx = 268;
	item->cy = 288;
	item->id = 200;
	ptr = (LPWORD)(item + 1);
	*ptr++ = 0xFFFF;
	*ptr++ = 0x0081; /* Edit */
	*ptr++ = 0;
	*ptr++ = 0;

	DialogBoxIndirectA(NULL, dlg, parent, about_dlg_proc);
}

static INT_PTR CALLBACK settings_dlg_proc(HWND dlg, UINT msg,
                                           WPARAM wp, LPARAM lp)
{
	UNUSED_PARAMETER(lp);

	switch (msg) {
	case WM_INITDIALOG: {
		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%u",
		         (unsigned)g_http_port);
		SetDlgItemTextA(dlg, IDC_PORT_EDIT, port_str);

		CheckDlgButton(dlg, IDC_AUTOSTART,
		               g_http_autostart ? BST_CHECKED
		                                : BST_UNCHECKED);

		update_status_label(dlg);
		return TRUE;
	}

	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDC_START_BTN: {
			char port_str[16];
			GetDlgItemTextA(dlg, IDC_PORT_EDIT, port_str,
			                sizeof(port_str));
			unsigned port = 0;
			if (sscanf(port_str, "%u", &port) == 1 &&
			    port > 0 && port < 65536) {
				g_http_port = (uint16_t)port;
			}

			if (!vjlink_http_server_is_running()) {
				char *file_path = obs_module_file(
					"web-ui/vjlink-control.html");
				if (file_path) {
					char *last_sep =
						strrchr(file_path, '/');
					if (!last_sep)
						last_sep = strrchr(
							file_path, '\\');
					if (last_sep)
						*last_sep = '\0';

					vjlink_http_server_start(
						g_http_port, file_path);
					bfree(file_path);
				} else {
					MessageBoxA(
						dlg,
						"web-ui files not found!\n"
						"Check plugin data directory.",
						"VJLink Error",
						MB_OK | MB_ICONERROR);
				}
			}
			update_status_label(dlg);
			return TRUE;
		}

		case IDC_STOP_BTN:
			vjlink_http_server_stop();
			update_status_label(dlg);
			return TRUE;

		case IDC_OPEN_UI_BTN: {
			char url[64];
			snprintf(url, sizeof(url), "http://localhost:%u",
			         (unsigned)vjlink_http_server_get_port());
			ShellExecuteA(NULL, "open", url, NULL, NULL,
			              SW_SHOWNORMAL);
			return TRUE;
		}

		case IDC_SAVE_BTN: {
			char port_str[16];
			GetDlgItemTextA(dlg, IDC_PORT_EDIT, port_str,
			                sizeof(port_str));
			unsigned port = 0;
			if (sscanf(port_str, "%u", &port) == 1 &&
			    port > 0 && port < 65536) {
				g_http_port = (uint16_t)port;
			}

			g_http_autostart =
				(IsDlgButtonChecked(dlg, IDC_AUTOSTART) ==
				 BST_CHECKED);

			save_config();
			MessageBoxA(dlg, "Settings saved!",
			            "VJLink", MB_OK | MB_ICONINFORMATION);
			return TRUE;
		}

		case IDC_ABOUT_BTN:
			show_about(dlg);
			return TRUE;

		case IDCANCEL:
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;

	case WM_CLOSE:
		EndDialog(dlg, 0);
		return TRUE;
	}

	return FALSE;
}

static LPWORD align_dword(LPWORD ptr)
{
	ULONG_PTR ul = (ULONG_PTR)ptr;
	ul = (ul + 3) & ~3;
	return (LPWORD)ul;
}

static LPWORD add_control(LPWORD ptr, DWORD style, short x, short y,
                           short cx, short cy, WORD id, WORD cls,
                           LPCWSTR text)
{
	ptr = align_dword(ptr);
	DLGITEMTEMPLATE *item = (DLGITEMTEMPLATE *)ptr;
	item->style = WS_CHILD | WS_VISIBLE | style;
	item->x = x;
	item->y = y;
	item->cx = cx;
	item->cy = cy;
	item->id = id;
	ptr = (LPWORD)(item + 1);
	*ptr++ = 0xFFFF;
	*ptr++ = cls;
	if (text) {
		int len = (int)wcslen(text) + 1;
		memcpy(ptr, text, len * sizeof(WCHAR));
		ptr += len;
	} else {
		*ptr++ = 0;
	}
	*ptr++ = 0;
	return ptr;
}

static void show_settings_dialog(void *data)
{
	UNUSED_PARAMETER(data);

	BYTE buf[4096];
	memset(buf, 0, sizeof(buf));

	DLGTEMPLATE *dlg = (DLGTEMPLATE *)buf;
	dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION |
	             WS_SYSMENU | DS_SETFONT;
	dlg->cdit = 9; /* number of controls */
	dlg->cx = 230;
	dlg->cy = 155;

	LPWORD ptr = (LPWORD)(dlg + 1);
	*ptr++ = 0; /* menu */
	*ptr++ = 0; /* class */

	LPCWSTR title = L"VJLink Settings";
	int tlen = (int)wcslen(title) + 1;
	memcpy(ptr, title, tlen * sizeof(WCHAR));
	ptr += tlen;
	*ptr++ = 9; /* font size */
	LPCWSTR font_name = L"Segoe UI";
	int flen = (int)wcslen(font_name) + 1;
	memcpy(ptr, font_name, flen * sizeof(WCHAR));
	ptr += flen;

	/* Control 1: Status label */
	ptr = add_control(ptr, SS_LEFT, 10, 10, 210, 12,
	                  IDC_STATUS_LABEL, 0x0082,
	                  L"Status: Unknown");

	/* Control 2: "HTTP Port:" label */
	ptr = add_control(ptr, SS_LEFT, 10, 32, 60, 12,
	                  0, 0x0082, L"HTTP Port:");

	/* Control 3: Port edit */
	ptr = add_control(ptr, WS_BORDER | WS_TABSTOP | ES_NUMBER,
	                  75, 30, 40, 14,
	                  IDC_PORT_EDIT, 0x0081, NULL);

	/* Control 4: Autostart checkbox */
	ptr = add_control(ptr, BS_AUTOCHECKBOX | WS_TABSTOP,
	                  10, 52, 200, 14,
	                  IDC_AUTOSTART, 0x0080,
	                  L"Auto-start with OBS");

	/* Control 5: Start button */
	ptr = add_control(ptr, BS_PUSHBUTTON | WS_TABSTOP,
	                  10, 76, 50, 18,
	                  IDC_START_BTN, 0x0080, L"Start");

	/* Control 6: Stop button */
	ptr = add_control(ptr, BS_PUSHBUTTON | WS_TABSTOP,
	                  65, 76, 50, 18,
	                  IDC_STOP_BTN, 0x0080, L"Stop");

	/* Control 7: Open Web UI button */
	ptr = add_control(ptr, BS_PUSHBUTTON | WS_TABSTOP,
	                  120, 76, 100, 18,
	                  IDC_OPEN_UI_BTN, 0x0080, L"Open Web UI");

	/* Control 8: Save button */
	ptr = add_control(ptr, BS_PUSHBUTTON | WS_TABSTOP,
	                  10, 104, 130, 20,
	                  IDC_SAVE_BTN, 0x0080, L"Save Settings");

	/* Control 9: About button */
	ptr = add_control(ptr, BS_PUSHBUTTON | WS_TABSTOP,
	                  145, 104, 75, 20,
	                  IDC_ABOUT_BTN, 0x0080, L"About...");

	DialogBoxIndirectA(NULL, dlg, GetActiveWindow(),
	                   settings_dlg_proc);
}

#endif /* _WIN32 */

/* --- Frontend API (dynamic loading) --- */

typedef void (*obs_frontend_cb)(void *private_data);
typedef void (*add_tools_menu_item_fn)(const char *, obs_frontend_cb,
                                        void *);

void vjlink_tools_menu_init(void)
{
	load_config();

#ifdef _WIN32
	HMODULE frontend = GetModuleHandleA("obs-frontend-api");
	if (!frontend) {
		blog(LOG_WARNING,
		     "[VJLink] obs-frontend-api not loaded, "
		     "Tools menu not available");
		return;
	}

	add_tools_menu_item_fn add_menu =
		(add_tools_menu_item_fn)GetProcAddress(
			frontend, "obs_frontend_add_tools_menu_item");

	if (!add_menu) {
		blog(LOG_WARNING,
		     "[VJLink] obs_frontend_add_tools_menu_item not found");
		return;
	}

	add_menu("VJLink Settings", show_settings_dialog, NULL);
	blog(LOG_INFO, "[VJLink] Tools menu item registered");
#endif
}

void vjlink_tools_apply_saved_settings(void)
{
	if (!g_http_autostart) {
		blog(LOG_INFO,
		     "[VJLink] HTTP auto-start disabled, "
		     "use Tools > VJLink Settings to start");
		return;
	}
}

uint16_t vjlink_tools_get_http_port(void)
{
	return g_http_port;
}

bool vjlink_tools_get_http_autostart(void)
{
	return g_http_autostart;
}

void vjlink_tools_menu_shutdown(void)
{
	/* Nothing to clean up */
}
