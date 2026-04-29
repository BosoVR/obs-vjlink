#pragma once

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations for subsystem pointers */
struct vjlink_band_effects;
struct vjlink_source_triggers;
struct vjlink_media_layers;

/* Audio texture dimensions */
#define VJLINK_AUDIO_TEX_WIDTH  512
#define VJLINK_AUDIO_TEX_HEIGHT 4
#define VJLINK_AUDIO_TEX_PIXELS (VJLINK_AUDIO_TEX_WIDTH * VJLINK_AUDIO_TEX_HEIGHT)

/* FFT configuration */
#define VJLINK_FFT_SIZE     2048
#define VJLINK_FFT_HOP      512
#define VJLINK_SAMPLE_RATE  48000

/* Frequency band count */
#define VJLINK_NUM_BANDS    4

/* Band indices */
#define VJLINK_BAND_BASS     0
#define VJLINK_BAND_LOWMID   1
#define VJLINK_BAND_HIGHMID  2
#define VJLINK_BAND_TREBLE   3

/* Maximum effect chain length */
#define VJLINK_MAX_EFFECTS  16

/* Maximum layers in compositor chain */
#define VJLINK_MAX_CHAIN    8

/* LFO count */
#define VJLINK_NUM_LFOS     4

/*
 * VJLinkContext - Global singleton shared by all modules.
 *
 * The audio thread writes to audio_cpu_buffer[write_idx].
 * The graphics thread reads from audio_cpu_buffer[1 - write_idx]
 * and uploads to audio_texture each frame.
 */
struct vjlink_context {
    /* GPU audio data texture (512x4 RGBA32F) */
    gs_texture_t    *audio_texture;
    bool             audio_texture_created;

    /* Compositor render target */
    gs_texrender_t  *compositor_output;
    uint32_t         compositor_width;
    uint32_t         compositor_height;

    /* CPU-side audio double buffer (RGBA float per pixel) */
    float            audio_cpu_buffer[2][VJLINK_AUDIO_TEX_PIXELS * 4];
    volatile long    audio_write_idx; /* 0 or 1, swapped atomically */

    /* Extracted audio features (written by audio thread, read by render) */
    volatile float   bands[VJLINK_NUM_BANDS];
    volatile float   bands_peak[VJLINK_NUM_BANDS];
    volatile float   bands_raw[VJLINK_NUM_BANDS];       /* unsmoothed raw */
    volatile float   chronotensity[VJLINK_NUM_BANDS];    /* cumulative energy (AudioLink-style) */
    volatile float   onset_strength;                      /* current onset detection value 0-1 */
    volatile float   beat_phase;
    volatile float   bpm;
    volatile float   beat_confidence;
    volatile float   rms;

    /* Per-band onset detection (kick=bass, snare=lowmid+highmid, hat=treble) */
    volatile float   kick_onset;     /* 0-1 spike on bass transient */
    volatile float   snare_onset;    /* 0-1 spike on mid transient */
    volatile float   hat_onset;      /* 0-1 spike on treble transient */

    /* BPM-derived phase subdivisions (0..1, wraps every N beats) */
    volatile float   beat_1_4;       /* phase across 1 beat   (= beat_phase) */
    volatile float   beat_1_8;       /* phase across 1/2 beat (twice per beat) */
    volatile float   beat_1_16;      /* phase across 1/4 beat */
    volatile float   beat_2_1;       /* phase across 2 beats  (half-time) */
    volatile float   beat_4_1;       /* phase across 4 beats  (one bar)   */
    volatile uint32_t beat_count;    /* monotonic beat counter */

    /* LFO values (updated on render thread) */
    float            lfo_values[VJLINK_NUM_LFOS];

    /* Elapsed time for shaders (tick-based accumulation, no wrapping) */
    float            elapsed_time;

    /* GPU capability info */
    int              gpu_quality;      /* 0=low, 1=medium, 2=high */
    bool             gpu_supports_float_tex;
    bool             gpu_checked;

    /* Logo textures (up to 3 user-selectable images) */
    gs_texture_t    *logo_texture;     /* logo_tex — primary */
    gs_texture_t    *logo_texture2;    /* logo_tex2 — secondary */
    gs_texture_t    *logo_texture3;    /* logo_tex3 — tertiary */

    /* WebSocket -> logo path override (slot 0,1,2) */
    char             pending_logo_path[512];
    char             pending_logo_path2[512];
    char             pending_logo_path3[512];
    volatile bool    logo_pending;
    volatile bool    logo_pending2;
    volatile bool    logo_pending3;

    /* WebSocket -> transparent bg override */
    bool             pending_transparent_bg;
    volatile bool    transparent_bg_pending;

    /* Pointers to active subsystems (set by compositor source) */
    struct vjlink_band_effects    *active_band_fx;
    struct vjlink_source_triggers *active_source_triggers;
    struct vjlink_media_layers    *active_media_layers;

    /* WebSocket -> Compositor effect override */
    char             pending_effect[64];
    volatile bool    effect_pending;
    /* Beat quantize for the pending effect:
     *   0 = immediate, 1 = on next beat, 4 = on next bar (4 beats),
     *   8 = on next 2 bars. Cleared on apply. */
    volatile int     pending_effect_quantize;
    /* Beat counter snapshot when the request arrived; used to wait
     * until (snapshot + quantize) boundary. */
    volatile uint32_t pending_effect_beat_anchor;

    /* Shader compile error log (last 8 entries, ring buffer) */
    char             shader_errors[8][256];
    volatile int     shader_error_write;     /* next write index */
    volatile int     shader_error_count;     /* total errors logged */

    /* Pending effect chain replacement (applied on render thread) */
    struct {
        char  effect_id[64];
        int   blend_mode;
        float blend_alpha;
    } pending_chain[8];
    volatile int     pending_chain_count;
    volatile bool    pending_chain_replace;
    int              active_preset_index;
    char             active_effect_id[64]; /* current effect for UI sync */
    float            band_sensitivity[4]; /* user gain per band, default 1.0 */
    float            audio_master_gain;    /* global gain before band normalize */
    float            audio_fall_rate;      /* smoothed band decay/fall speed */
    int              palette_id;           /* 0=Default 1=Hardtechno 2=Rawstyle 3=Acid 4=Cyber 5=Mono */
    float            macro_energy;          /* 0..1 — Performance macro: drives intensity/contrast */
    float            macro_chaos;           /* 0..1 — Performance macro: glitch/noise/distortion */
    float            macro_speed;           /* 0..1 — Performance macro: speed multiplier */
    float            macro_color;           /* 0..1 — Performance macro: hue/saturation shift */
    float            strobe_safety_max;     /* 0..1 — Max strobe brightness, 1.0 = no limit */

    /* WebSocket -> Compositor param override (applied on render thread) */
#define VJLINK_MAX_PENDING_PARAMS 32
    struct {
        char  name[64];
        float value;
    } pending_params[VJLINK_MAX_PENDING_PARAMS];
    volatile int     pending_param_count;

    /* Last shader compile error (for UI display) */
    char             last_error[512];
    volatile bool    has_error;

    /* Thread synchronization */
    pthread_mutex_t  mutex;
    bool             initialized;
};

/* GPU quality levels */
#define VJLINK_GPU_LOW    0
#define VJLINK_GPU_MEDIUM 1
#define VJLINK_GPU_HIGH   2

/* Get global context (creates on first call) */
struct vjlink_context *vjlink_get_context(void);

/* Initialize / shutdown */
bool vjlink_context_init(void);
void vjlink_context_shutdown(void);

/* Append a shader compile/load error message to the ring buffer.
 * Truncated to 255 chars. Visible in Web UI Diagnostics panel. */
void vjlink_context_log_shader_error(const char *msg);

/* Check GPU capabilities (call once from graphics thread) */
void vjlink_check_gpu_caps(void);

/* Swap audio buffer (called by audio thread after writing) */
void vjlink_audio_buffer_swap(void);

/* Get the read-side audio buffer (for GPU upload) */
float *vjlink_audio_buffer_read(void);

/* Accumulate elapsed time (call from video_tick with delta seconds) */
void vjlink_tick_time(float seconds);

#ifdef __cplusplus
}
#endif
