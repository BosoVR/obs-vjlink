#include "vjlink_context.h"
#include <string.h>
#include <obs-module.h>

#ifdef _WIN32
#include <windows.h>
#define ATOMIC_SWAP(ptr, val) InterlockedExchange((volatile LONG *)(ptr), (LONG)(val))
#define ATOMIC_READ(ptr)      InterlockedCompareExchange((volatile LONG *)(ptr), 0, 0)
#else
#define ATOMIC_SWAP(ptr, val) __sync_lock_test_and_set(ptr, val)
#define ATOMIC_READ(ptr)      __sync_add_and_fetch(ptr, 0)
#endif

static struct vjlink_context g_vjlink_ctx;

struct vjlink_context *vjlink_get_context(void)
{
	return &g_vjlink_ctx;
}

bool vjlink_context_init(void)
{
	struct vjlink_context *ctx = &g_vjlink_ctx;

	if (ctx->initialized)
		return true;

	memset(ctx, 0, sizeof(*ctx));

	if (pthread_mutex_init(&ctx->mutex, NULL) != 0) {
		blog(LOG_ERROR, "[VJLink] Failed to init mutex");
		return false;
	}

	ctx->audio_write_idx = 0;
	ctx->compositor_width = 1920;
	ctx->compositor_height = 1080;
	ctx->elapsed_time = 0.0f;
	for (int i = 0; i < 4; i++)
		ctx->band_sensitivity[i] = 1.0f;
	ctx->initialized = true;

	blog(LOG_INFO, "[VJLink] Context initialized");
	return true;
}

void vjlink_context_shutdown(void)
{
	struct vjlink_context *ctx = &g_vjlink_ctx;

	if (!ctx->initialized)
		return;

	/*
	 * During obs_shutdown -> free_module, the graphics subsystem may
	 * already be torn down. OBS cleans up all GPU resources automatically,
	 * so we just NULL our pointers to avoid double-free crashes.
	 */
	ctx->audio_texture = NULL;
	ctx->audio_texture_created = false;
	ctx->compositor_output = NULL;

	pthread_mutex_destroy(&ctx->mutex);
	ctx->initialized = false;

	blog(LOG_INFO, "[VJLink] Context shutdown");
}

void vjlink_audio_buffer_swap(void)
{
	struct vjlink_context *ctx = &g_vjlink_ctx;
	long current = ATOMIC_READ(&ctx->audio_write_idx);
	ATOMIC_SWAP(&ctx->audio_write_idx, 1 - current);
}

float *vjlink_audio_buffer_read(void)
{
	struct vjlink_context *ctx = &g_vjlink_ctx;
	long write_idx = ATOMIC_READ(&ctx->audio_write_idx);
	/* Read from the buffer NOT being written to */
	return ctx->audio_cpu_buffer[1 - write_idx];
}

void vjlink_check_gpu_caps(void)
{
	struct vjlink_context *ctx = &g_vjlink_ctx;

	if (ctx->gpu_checked)
		return;

	ctx->gpu_checked = true;
	ctx->gpu_quality = VJLINK_GPU_HIGH;
	ctx->gpu_supports_float_tex = false;

	/* Test float texture support by creating a small RGBA32F texture */
	gs_texture_t *test = gs_texture_create(4, 4, GS_RGBA32F, 1, NULL, 0);
	if (test) {
		gs_texture_destroy(test);
		ctx->gpu_supports_float_tex = true;
		blog(LOG_INFO, "[VJLink] GPU supports RGBA32F textures");
	} else {
		ctx->gpu_supports_float_tex = false;
		ctx->gpu_quality = VJLINK_GPU_LOW;
		blog(LOG_WARNING, "[VJLink] GPU does NOT support RGBA32F - "
		     "using reduced quality mode");
	}

	/* Log GPU info from OBS graphics subsystem */
	const char *renderer = gs_get_device_name();
	if (renderer) {
		blog(LOG_INFO, "[VJLink] GPU: %s", renderer);

		/* Heuristic: detect low-end GPUs by name */
		if (strstr(renderer, "Intel HD") ||
		    strstr(renderer, "Intel UHD") ||
		    strstr(renderer, "Intel(R) HD") ||
		    strstr(renderer, "Intel(R) UHD")) {
			if (ctx->gpu_quality > VJLINK_GPU_LOW)
				ctx->gpu_quality = VJLINK_GPU_MEDIUM;
			blog(LOG_INFO, "[VJLink] Intel integrated GPU detected "
			     "- using medium quality");
		}
	}

	blog(LOG_INFO, "[VJLink] GPU quality level: %s",
	     ctx->gpu_quality == VJLINK_GPU_HIGH ? "High" :
	     ctx->gpu_quality == VJLINK_GPU_MEDIUM ? "Medium" : "Low");
}

void vjlink_tick_time(float seconds)
{
	struct vjlink_context *ctx = &g_vjlink_ctx;
	if (seconds > 0.0f && seconds < 1.0f)
		ctx->elapsed_time += seconds;
}
