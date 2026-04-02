#include <obs-module.h>
#include <string.h>
#include "vjlink_context.h"
#include "rendering/effect_system.h"
#include "presets/preset_manager.h"
#include "controls/hotkey_manager.h"
#include "controls/websocket_handler.h"
#include "controls/http_server.h"
#include "controls/tools_menu.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-vjlink", "en-US")

/* Forward declarations for source registrations */
extern struct obs_source_info vjlink_audio_filter_info;
extern struct obs_source_info vjlink_compositor_source_info;
extern struct obs_source_info vjlink_effect_filter_info;
extern struct obs_source_info vjlink_videowall_source_info;

bool obs_module_load(void)
{
	blog(LOG_INFO, "[VJLink] Loading OBS-VJLink v%s", "1.0.0");

	/* Initialize global context */
	if (!vjlink_context_init()) {
		blog(LOG_ERROR, "[VJLink] Failed to initialize context");
		return false;
	}

	/* Initialize effect system */
	vjlink_effect_system_init();

	/* Initialize preset manager */
	vjlink_preset_manager_init();

	/* Register OBS sources/filters */
	obs_register_source(&vjlink_audio_filter_info);
	obs_register_source(&vjlink_compositor_source_info);
	obs_register_source(&vjlink_effect_filter_info);
	obs_register_source(&vjlink_videowall_source_info);

	/* Initialize control modules */
	vjlink_hotkeys_init();
	vjlink_websocket_init();

	/* Initialize tools menu (loads config, adds Tools menu entry) */
	vjlink_tools_menu_init();

	/* Start built-in HTTP server for web-ui (if auto-start enabled) */
	if (vjlink_tools_get_http_autostart()) {
		/* obs_module_file returns path to a file inside plugin data.
		 * We ask for a known file, then strip the filename to get
		 * the web-ui directory path. */
		char *file_path = obs_module_file(
			"web-ui/vjlink-control.html");
		if (file_path) {
			/* Strip "/vjlink-control.html" to get directory */
			char *last_sep = strrchr(file_path, '/');
			if (!last_sep)
				last_sep = strrchr(file_path, '\\');
			if (last_sep)
				*last_sep = '\0';

			blog(LOG_INFO, "[VJLink] web-ui path: %s", file_path);
			vjlink_http_server_start(
				vjlink_tools_get_http_port(), file_path);
			bfree(file_path);
		} else {
			blog(LOG_WARNING,
			     "[VJLink] web-ui/vjlink-control.html not found, "
			     "HTTP server disabled");
		}
	} else {
		blog(LOG_INFO,
		     "[VJLink] HTTP auto-start disabled, "
		     "use Tools > VJLink Settings to start");
	}

	blog(LOG_INFO, "[VJLink] Plugin loaded successfully "
	     "(4 sources, hotkeys, websocket, http:%u)",
	     vjlink_http_server_get_port());
	return true;
}

static void delayed_websocket_init(void *param, float unused)
{
	UNUSED_PARAMETER(unused);
	/* obs-websocket registers its procs in obs_module_post_load,
	 * which may run after ours. A short tick delay ensures it's ready. */
	vjlink_websocket_late_init();
	/* Remove this tick callback after first run */
	obs_remove_tick_callback(delayed_websocket_init, param);
}

void obs_module_post_load(void)
{
	/* First try immediately (in case obs-websocket loaded before us) */
	vjlink_websocket_late_init();
	/* If that failed, schedule a retry on the next tick */
	obs_add_tick_callback(delayed_websocket_init, NULL);
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[VJLink] Unloading plugin");
	vjlink_tools_menu_shutdown();
	vjlink_http_server_stop();
	vjlink_websocket_shutdown();
	vjlink_hotkeys_shutdown();
	vjlink_preset_manager_shutdown();
	vjlink_effect_system_shutdown();
	vjlink_context_shutdown();
}

const char *obs_module_name(void)
{
	return "OBS-VJLink";
}

const char *obs_module_description(void)
{
	return "VJ visuals and audio-reactive effects for OBS Studio";
}
