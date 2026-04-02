#pragma once

#include "vjlink_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Audio Texture Manager
 *
 * Handles creation and per-frame upload of the 512x4 RGBA32F
 * GPU texture that shaders sample for audio data.
 *
 * Must be called from the OBS graphics thread (inside video_render).
 */

/* Create the GPU texture (call once from graphics thread) */
bool vjlink_audio_texture_create(void);

/* Upload CPU buffer to GPU texture (call each frame from graphics thread) */
void vjlink_audio_texture_upload(void);

/* Get the GPU texture for binding as shader uniform */
gs_texture_t *vjlink_audio_texture_get(void);

/* Destroy the GPU texture (call from graphics thread) */
void vjlink_audio_texture_destroy(void);

#ifdef __cplusplus
}
#endif
