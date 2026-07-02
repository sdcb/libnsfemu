#ifndef NSFEMU_H
#define NSFEMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(NSFEMU_SHARED)
#  ifdef NSFEMU_BUILDING
#    define NSFEMU_API __declspec(dllexport)
#  else
#    define NSFEMU_API __declspec(dllimport)
#  endif
#else
#  define NSFEMU_API
#endif

typedef struct nsfemu nsfemu_t;

typedef enum nsfemu_error {
    NSFEMU_OK = 0,
    NSFEMU_ERROR_INVALID_ARGUMENT = 1,
    NSFEMU_ERROR_OUT_OF_MEMORY = 2,
    NSFEMU_ERROR_BAD_NSF = 3,
    NSFEMU_ERROR_UNSUPPORTED_EXPANSION = 4,
    NSFEMU_ERROR_UNSUPPORTED_SAMPLE_RATE = 5,
    NSFEMU_ERROR_TRACK_OUT_OF_RANGE = 6,
    NSFEMU_ERROR_NOT_LOADED = 7,
    NSFEMU_ERROR_NOT_STARTED = 8
} nsfemu_error_t;

typedef enum nsfemu_load_flags {
    NSFEMU_LOAD_DEFAULT = 0,
    NSFEMU_LOAD_REJECT_UNSUPPORTED_EXPANSION = 1u << 0
} nsfemu_load_flags_t;

typedef enum nsfemu_channel {
    NSFEMU_CHANNEL_SQUARE1  = 1u << 0,
    NSFEMU_CHANNEL_SQUARE2  = 1u << 1,
    NSFEMU_CHANNEL_TRIANGLE = 1u << 2,
    NSFEMU_CHANNEL_NOISE    = 1u << 3,
    NSFEMU_CHANNEL_DPCM     = 1u << 4,
    NSFEMU_CHANNEL_ALL      = 0x1Fu
} nsfemu_channel_t;

typedef struct nsfemu_info {
    uint8_t version;
    uint8_t track_count;
    uint8_t first_track;
    uint16_t load_addr;
    uint16_t init_addr;
    uint16_t play_addr;
    uint8_t speed_flags;
    uint8_t chip_flags;
    uint8_t banks[8];
    char title[33];
    char artist[33];
    char copyright[33];
} nsfemu_info_t;

typedef enum nsfemu_length_probe_status {
    NSFEMU_LENGTH_PROBE_ENDED = 0,
    NSFEMU_LENGTH_PROBE_ACTIVE_AT_LIMIT = 1,
    NSFEMU_LENGTH_PROBE_NO_WRITES = 2
} nsfemu_length_probe_status_t;

typedef struct nsfemu_length_probe {
    nsfemu_length_probe_status_t status;
    uint32_t probe_milliseconds;
    uint32_t last_write_milliseconds;
    uint32_t frames_observed;
    uint32_t writes_observed;
} nsfemu_length_probe_t;

typedef enum nsfemu_activity_status {
    NSFEMU_ACTIVITY_NO_WRITES = 0,
    NSFEMU_ACTIVITY_ACTIVE = 1,
    NSFEMU_ACTIVITY_ENDED = 2
} nsfemu_activity_status_t;

typedef struct nsfemu_activity {
    nsfemu_activity_status_t status;
    uint32_t quiet_frames;
    uint32_t current_play_frame;
    uint32_t last_write_play_frame;
    uint32_t writes_observed;
} nsfemu_activity_t;

typedef void (*nsfemu_apu_write_callback_t)(
    void *user_data,
    int frame,
    int sequence,
    int address,
    int value);

NSFEMU_API const char *nsfemu_error_string(nsfemu_error_t error);

NSFEMU_API nsfemu_error_t nsfemu_create(nsfemu_t **out);
NSFEMU_API void nsfemu_destroy(nsfemu_t *emu);

NSFEMU_API nsfemu_error_t nsfemu_set_sample_rate(nsfemu_t *emu, int sample_rate);
NSFEMU_API int nsfemu_sample_rate(const nsfemu_t *emu);

NSFEMU_API nsfemu_error_t nsfemu_load_memory(
    nsfemu_t *emu,
    const void *data,
    size_t size,
    uint32_t load_flags);

NSFEMU_API nsfemu_error_t nsfemu_get_info(const nsfemu_t *emu, nsfemu_info_t *out);

/* track is zero-based, matching libgme's gme_start_track(). */
NSFEMU_API nsfemu_error_t nsfemu_start_track(nsfemu_t *emu, int track);

NSFEMU_API void nsfemu_set_channel_mask(nsfemu_t *emu, uint32_t channel_mask);
NSFEMU_API uint32_t nsfemu_channel_mask(const nsfemu_t *emu);

NSFEMU_API void nsfemu_set_apu_write_callback(
    nsfemu_t *emu,
    nsfemu_apu_write_callback_t callback,
    void *user_data);

/*
 * Optional runtime activity tracking. When enabled, APU register writes are
 * counted during normal rendering so callers can detect tracks that have gone
 * quiet without changing nsfemu_render_s16le()'s PCM-fill contract.
 */
NSFEMU_API void nsfemu_set_activity_tracking(nsfemu_t *emu, int enabled);
NSFEMU_API int nsfemu_activity_tracking(const nsfemu_t *emu);
NSFEMU_API nsfemu_error_t nsfemu_get_activity(
    const nsfemu_t *emu,
    uint32_t quiet_frames,
    nsfemu_activity_t *out);

/*
 * Render signed 16-bit little-endian PCM.
 * channels must be 1 or 2; stereo is produced by duplicating mono to L/R.
 * out_size must be at least frame_count * channels * 2 bytes.
 */
NSFEMU_API nsfemu_error_t nsfemu_render_s16le(
    nsfemu_t *emu,
    void *out,
    size_t out_size,
    size_t frame_count,
    int channels);

/*
 * Best-effort CPU-only duration/activity probe. It runs NSF init/play code and
 * records APU writes up to probe_milliseconds, but does not run APU synthesis
 * and does not generate PCM.
 * track is zero-based, matching nsfemu_start_track().
 */
NSFEMU_API nsfemu_error_t nsfemu_probe_length(
    const void *data,
    size_t size,
    int track,
    uint32_t probe_milliseconds,
    nsfemu_length_probe_t *out);

#ifdef __cplusplus
}
#endif

#endif
