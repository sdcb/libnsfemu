#ifndef NSFEMU_BLIP_BUFFER_H
#define NSFEMU_BLIP_BUFFER_H

#include <stdint.h>

typedef int blip_long;
typedef unsigned blip_ulong;
typedef blip_long blip_time_t;
typedef int16_t blip_sample_t;
typedef blip_ulong blip_resampled_time_t;

enum {
    BLIP_BUFFER_ACCURACY = 16,
    BLIP_PHASE_BITS = 6,
    BLIP_RES = 1 << BLIP_PHASE_BITS,
    BLIP_WIDEST_IMPULSE = 16,
    BLIP_BUFFER_EXTRA = BLIP_WIDEST_IMPULSE + 2,
    BLIP_SAMPLE_BITS = 30,
    BLIP_MED_QUALITY = 8,
    BLIP_GOOD_QUALITY = 12,
    BLIP_MAX_QUALITY = 12,
    BLIP_DEFAULT_LENGTH = 250,
    BLIP_MAX_LENGTH = 0
};

typedef struct blip_eq {
    double treble;
    long rolloff_freq;
    long sample_rate;
    long cutoff_freq;
} blip_eq_t;

typedef struct blip_buffer {
    blip_ulong factor;
    blip_resampled_time_t offset;
    blip_long *buffer;
    blip_long buffer_size;
    blip_long reader_accum;
    int bass_shift;
    long sample_rate;
    uint32_t clock_rate;
    int bass_freq;
    int length;
    int modified;
} blip_buffer_t;

typedef struct blip_synth {
    blip_buffer_t *buf;
    int last_amp;
    int delta_factor;
    double volume_unit;
    blip_long kernel_unit;
    int quality;
    int width;
    int impulse_count;
    int range;
    int initialized;
    int16_t impulses[BLIP_RES * (BLIP_MAX_QUALITY / 2) + 1];
} blip_synth_t;

void blip_eq_init(blip_eq_t *eq, double treble);
void blip_eq_init_full(blip_eq_t *eq, double treble, long rolloff_freq, long sample_rate, long cutoff_freq);

void blip_buffer_init(blip_buffer_t *buf);
void blip_buffer_destroy(blip_buffer_t *buf);
const char *blip_buffer_set_sample_rate(blip_buffer_t *buf, long sample_rate, int msec);
void blip_buffer_clock_rate(blip_buffer_t *buf, uint32_t clocks_per_second);
void blip_buffer_bass_freq(blip_buffer_t *buf, int frequency);
void blip_buffer_clear(blip_buffer_t *buf, int entire_buffer);
void blip_buffer_end_frame(blip_buffer_t *buf, blip_time_t time);
long blip_buffer_read_samples(blip_buffer_t *buf, blip_sample_t *dest, long max_samples);
long blip_buffer_read_samples_interleaved(blip_buffer_t *buf, blip_sample_t *dest, long max_frames, int channels);
long blip_buffer_samples_avail(const blip_buffer_t *buf);
long blip_buffer_count_samples(const blip_buffer_t *buf, blip_time_t duration);
blip_time_t blip_buffer_count_clocks(const blip_buffer_t *buf, long count);
void blip_buffer_remove_samples(blip_buffer_t *buf, long count);
void blip_buffer_remove_silence(blip_buffer_t *buf, long count);
void blip_buffer_set_modified(blip_buffer_t *buf);
int blip_buffer_clear_modified(blip_buffer_t *buf);
blip_resampled_time_t blip_buffer_resampled_duration(const blip_buffer_t *buf, int time);
blip_resampled_time_t blip_buffer_resampled_time(const blip_buffer_t *buf, blip_time_t time);

void blip_synth_init(blip_synth_t *synth, int quality, int range);
void blip_synth_volume(blip_synth_t *synth, double volume);
void blip_synth_treble_eq(blip_synth_t *synth, const blip_eq_t *eq);
void blip_synth_offset(blip_synth_t *synth, blip_time_t time, int delta, blip_buffer_t *buf);
void blip_synth_offset_inline(blip_synth_t *synth, blip_time_t time, int delta, blip_buffer_t *buf);
void blip_synth_offset_resampled(blip_synth_t *synth, blip_resampled_time_t time, int delta, blip_buffer_t *buf);

#endif
