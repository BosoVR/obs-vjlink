#include "audio_texture.h"
#include <obs-module.h>
#include <string.h>

bool vjlink_audio_texture_create(void)
{
	struct vjlink_context *ctx = vjlink_get_context();

	if (ctx->audio_texture_created)
		return true;

	/*
	 * Create a 512x4 RGBA32F texture.
	 * GS_RGBA32F = 32-bit float per channel, 4 channels.
	 * GS_DYNAMIC = will be updated frequently via gs_texture_set_image.
	 */
	ctx->audio_texture = gs_texture_create(
		VJLINK_AUDIO_TEX_WIDTH,
		VJLINK_AUDIO_TEX_HEIGHT,
		GS_RGBA32F,
		1,    /* num_levels (no mipmaps) */
		NULL, /* initial data (none) */
		GS_DYNAMIC
	);

	if (!ctx->audio_texture) {
		blog(LOG_ERROR, "[VJLink] Failed to create audio texture (%dx%d RGBA32F)",
		     VJLINK_AUDIO_TEX_WIDTH, VJLINK_AUDIO_TEX_HEIGHT);
		return false;
	}

	ctx->audio_texture_created = true;
	blog(LOG_INFO, "[VJLink] Audio texture created (%dx%d RGBA32F)",
	     VJLINK_AUDIO_TEX_WIDTH, VJLINK_AUDIO_TEX_HEIGHT);
	return true;
}

void vjlink_audio_texture_upload(void)
{
	struct vjlink_context *ctx = vjlink_get_context();

	if (!ctx->audio_texture)
		return;

	/* Get the read-side buffer (not currently being written by audio thread) */
	float *data = vjlink_audio_buffer_read();

	/*
	 * Upload to GPU.
	 * Row pitch = width * 4 channels * sizeof(float)
	 */
	uint32_t row_pitch = VJLINK_AUDIO_TEX_WIDTH * 4 * sizeof(float);
	const uint8_t *src = (const uint8_t *)data;

	gs_texture_set_image(ctx->audio_texture, src, row_pitch, false);
}

gs_texture_t *vjlink_audio_texture_get(void)
{
	struct vjlink_context *ctx = vjlink_get_context();
	return ctx->audio_texture;
}

void vjlink_audio_texture_destroy(void)
{
	struct vjlink_context *ctx = vjlink_get_context();

	if (ctx->audio_texture) {
		gs_texture_destroy(ctx->audio_texture);
		ctx->audio_texture = NULL;
		ctx->audio_texture_created = false;
		blog(LOG_INFO, "[VJLink] Audio texture destroyed");
	}
}
