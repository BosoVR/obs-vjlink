#include "hotkey_manager.h"
#include "vjlink_context.h"
#include "presets/preset_manager.h"
#include <obs-module.h>
#include <string.h>
#include <stdlib.h>

/*
 * VJLink Hotkey Manager
 *
 * Registers frontend hotkeys for live performance control.
 * All hotkeys are configurable through OBS Settings > Hotkeys.
 */

/* Internal state */
static obs_hotkey_id g_hotkey_next_preset = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_hotkey_prev_preset = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_hotkey_tap_beat = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_hotkey_blackout = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_hotkey_preset_slots[10];

static int  g_current_preset_index = -1;
static bool g_blackout = false;

/* Beat tap state for manual BPM */
#define TAP_HISTORY_SIZE 8
static uint64_t g_tap_times[TAP_HISTORY_SIZE];
static int      g_tap_count = 0;
static int      g_tap_write_idx = 0;

/* --- Beat Tap --- */

void vjlink_tap_beat(void)
{
	uint64_t now = os_gettime_ns();

	g_tap_times[g_tap_write_idx] = now;
	g_tap_write_idx = (g_tap_write_idx + 1) % TAP_HISTORY_SIZE;
	if (g_tap_count < TAP_HISTORY_SIZE)
		g_tap_count++;

	/* Need at least 2 taps to compute BPM */
	if (g_tap_count < 2)
		return;

	/* Average interval from recent taps */
	double total_interval = 0.0;
	int intervals = 0;

	for (int i = 1; i < g_tap_count; i++) {
		int idx_curr = (g_tap_write_idx - 1 - i + TAP_HISTORY_SIZE * 2) %
			TAP_HISTORY_SIZE;
		int idx_prev = (idx_curr - 1 + TAP_HISTORY_SIZE) %
			TAP_HISTORY_SIZE;

		/* Ignore if not enough history */
		if (g_tap_times[idx_curr] == 0 || g_tap_times[idx_prev] == 0)
			continue;

		double interval_ns = (double)(g_tap_times[idx_curr] -
			g_tap_times[idx_prev]);
		double interval_sec = interval_ns / 1000000000.0;

		/* Ignore unreasonable intervals */
		if (interval_sec > 0.2 && interval_sec < 3.0) {
			total_interval += interval_sec;
			intervals++;
		}
	}

	if (intervals > 0) {
		double avg_interval = total_interval / (double)intervals;
		float bpm = (float)(60.0 / avg_interval);

		/* Clamp to reasonable range */
		if (bpm >= 30.0f && bpm <= 300.0f) {
			struct vjlink_context *ctx = vjlink_get_context();
			ctx->bpm = bpm;
			ctx->beat_confidence = 1.0f;
			ctx->beat_phase = 0.0f; /* reset phase on tap */

			blog(LOG_INFO, "[VJLink] Manual BPM tap: %.1f BPM",
			     bpm);
		}
	}
}

/* --- Preset Navigation --- */

void vjlink_preset_next(void)
{
	int count = vjlink_preset_manager_get_count();
	if (count <= 0)
		return;

	g_current_preset_index = (g_current_preset_index + 1) % count;
	vjlink_preset_apply_by_index(g_current_preset_index);

	blog(LOG_INFO, "[VJLink] Next preset: %d / %d",
	     g_current_preset_index + 1, count);
}

void vjlink_preset_prev(void)
{
	int count = vjlink_preset_manager_get_count();
	if (count <= 0)
		return;

	g_current_preset_index = (g_current_preset_index - 1 + count) % count;
	vjlink_preset_apply_by_index(g_current_preset_index);

	blog(LOG_INFO, "[VJLink] Previous preset: %d / %d",
	     g_current_preset_index + 1, count);
}

void vjlink_preset_set_index(int index)
{
	/* Allow -1 to clear preset selection */
	if (index < 0) {
		g_current_preset_index = -1;
		return;
	}

	int count = vjlink_preset_manager_get_count();
	if (count <= 0 || index >= count)
		return;

	g_current_preset_index = index;
	vjlink_preset_apply_by_index(g_current_preset_index);
}

void vjlink_toggle_blackout(void)
{
	g_blackout = !g_blackout;
	blog(LOG_INFO, "[VJLink] Blackout: %s",
	     g_blackout ? "ON" : "OFF");
}

bool vjlink_is_blackout(void)
{
	return g_blackout;
}

int vjlink_get_current_preset_index(void)
{
	return g_current_preset_index;
}

/* --- Hotkey Callbacks --- */

static void hotkey_next_preset(void *data, obs_hotkey_id id,
                                obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		vjlink_preset_next();
}

static void hotkey_prev_preset(void *data, obs_hotkey_id id,
                                obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		vjlink_preset_prev();
}

static void hotkey_tap_beat(void *data, obs_hotkey_id id,
                             obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		vjlink_tap_beat();
}

static void hotkey_blackout(void *data, obs_hotkey_id id,
                             obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		vjlink_toggle_blackout();
}

static void hotkey_preset_slot(void *data, obs_hotkey_id id,
                                obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;

	int slot = (int)(intptr_t)data;
	vjlink_preset_set_index(slot);
	blog(LOG_INFO, "[VJLink] Preset slot %d activated", slot + 1);
}

/* --- Init / Shutdown --- */

void vjlink_hotkeys_init(void)
{
	/* Next preset */
	g_hotkey_next_preset = obs_hotkey_register_frontend(
		"vjlink_next_preset",
		obs_module_text("VJLink.Hotkey.NextPreset"),
		hotkey_next_preset, NULL);

	/* Previous preset */
	g_hotkey_prev_preset = obs_hotkey_register_frontend(
		"vjlink_prev_preset",
		obs_module_text("VJLink.Hotkey.PrevPreset"),
		hotkey_prev_preset, NULL);

	/* Beat tap */
	g_hotkey_tap_beat = obs_hotkey_register_frontend(
		"vjlink_tap_beat",
		obs_module_text("VJLink.Hotkey.TapBeat"),
		hotkey_tap_beat, NULL);

	/* Blackout toggle */
	g_hotkey_blackout = obs_hotkey_register_frontend(
		"vjlink_blackout",
		obs_module_text("VJLink.Hotkey.Blackout"),
		hotkey_blackout, NULL);

	/* Preset slots 1-10 */
	for (int i = 0; i < 10; i++) {
		char name[64];
		char desc[64];
		snprintf(name, sizeof(name), "vjlink_preset_slot_%d", i + 1);
		snprintf(desc, sizeof(desc), "VJLink: Preset Slot %d", i + 1);

		g_hotkey_preset_slots[i] = obs_hotkey_register_frontend(
			name, desc,
			hotkey_preset_slot, (void *)(intptr_t)i);
	}

	/* Initialize tap state */
	memset(g_tap_times, 0, sizeof(g_tap_times));
	g_tap_count = 0;
	g_tap_write_idx = 0;

	blog(LOG_INFO, "[VJLink] Hotkeys registered (4 + 10 preset slots)");
}

void vjlink_hotkeys_shutdown(void)
{
	if (g_hotkey_next_preset != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_hotkey_next_preset);
	if (g_hotkey_prev_preset != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_hotkey_prev_preset);
	if (g_hotkey_tap_beat != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_hotkey_tap_beat);
	if (g_hotkey_blackout != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_hotkey_blackout);

	for (int i = 0; i < 10; i++) {
		if (g_hotkey_preset_slots[i] != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(g_hotkey_preset_slots[i]);
	}

	blog(LOG_INFO, "[VJLink] Hotkeys unregistered");
}
