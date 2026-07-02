#include "blip_buffer.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795029
#endif

static blip_resampled_time_t blip_clock_rate_factor(const blip_buffer_t *buf, uint32_t rate)
{
    double ratio = (double)buf->sample_rate / rate;
    blip_long factor = (blip_long)floor(ratio * (1L << BLIP_BUFFER_ACCURACY) + 0.5);
    assert(factor > 0 || !buf->sample_rate);
    return (blip_resampled_time_t)factor;
}

void blip_eq_init(blip_eq_t *eq, double treble)
{
    eq->treble = treble;
    eq->rolloff_freq = 0;
    eq->sample_rate = 44100;
    eq->cutoff_freq = 0;
}

void blip_eq_init_full(blip_eq_t *eq, double treble, long rolloff_freq, long sample_rate, long cutoff_freq)
{
    eq->treble = treble;
    eq->rolloff_freq = rolloff_freq;
    eq->sample_rate = sample_rate;
    eq->cutoff_freq = cutoff_freq;
}

void blip_buffer_init(blip_buffer_t *buf)
{
    memset(buf, 0, sizeof(*buf));
    buf->factor = (blip_ulong)-1 / 2;
    buf->bass_freq = 16;
}

void blip_buffer_destroy(blip_buffer_t *buf)
{
    free(buf->buffer);
    memset(buf, 0, sizeof(*buf));
}

long blip_buffer_samples_avail(const blip_buffer_t *buf)
{
    long samples = (long)(buf->offset >> BLIP_BUFFER_ACCURACY);
    return samples <= buf->buffer_size ? samples : 0;
}

void blip_buffer_clear(blip_buffer_t *buf, int entire_buffer)
{
    buf->offset = 0;
    buf->reader_accum = 0;
    buf->modified = 0;
    if (buf->buffer) {
        long count = entire_buffer ? buf->buffer_size : blip_buffer_samples_avail(buf);
        memset(buf->buffer, 0, (size_t)(count + BLIP_BUFFER_EXTRA) * sizeof(buf->buffer[0]));
    }
}

const char *blip_buffer_set_sample_rate(blip_buffer_t *buf, long new_rate, int msec)
{
    long new_size = (long)((UINT_MAX >> BLIP_BUFFER_ACCURACY) - BLIP_BUFFER_EXTRA - 64);
    if (msec != BLIP_MAX_LENGTH) {
        /* Allocate one extra millisecond like Blip_Buffer so end-of-frame
           rounding cannot overflow the impulse tail. */
        long s = (new_rate * (msec + 1) + 999) / 1000;
        if (s < new_size) {
            new_size = s;
        } else {
            assert(0);
        }
    }

    if (buf->buffer_size != new_size) {
        void *p = realloc(buf->buffer, (size_t)(new_size + BLIP_BUFFER_EXTRA) * sizeof(buf->buffer[0]));
        if (!p) {
            return "Out of memory";
        }
        buf->buffer = (blip_long *)p;
    }

    buf->buffer_size = new_size;
    buf->sample_rate = new_rate;
    buf->length = (int)(new_size * 1000 / new_rate - 1);
    if (buf->clock_rate) {
        blip_buffer_clock_rate(buf, buf->clock_rate);
    }
    blip_buffer_bass_freq(buf, buf->bass_freq);
    blip_buffer_clear(buf, 1);
    return NULL;
}

void blip_buffer_clock_rate(blip_buffer_t *buf, uint32_t clocks_per_second)
{
    buf->clock_rate = clocks_per_second;
    buf->factor = blip_clock_rate_factor(buf, clocks_per_second);
}

void blip_buffer_bass_freq(blip_buffer_t *buf, int frequency)
{
    buf->bass_freq = frequency;
    int shift = 31;
    if (frequency > 0) {
        shift = 13;
        long f = ((long)frequency << 16) / buf->sample_rate;
        while ((f >>= 1) && --shift) {
        }
    }
    /* bass_shift implements the cheap high-pass filter used while reading. */
    buf->bass_shift = shift;
}

void blip_buffer_end_frame(blip_buffer_t *buf, blip_time_t time)
{
    buf->offset += (blip_resampled_time_t)time * buf->factor;
}

void blip_buffer_remove_silence(blip_buffer_t *buf, long count)
{
    assert(count <= blip_buffer_samples_avail(buf));
    buf->offset -= (blip_resampled_time_t)count << BLIP_BUFFER_ACCURACY;
}

void blip_buffer_remove_samples(blip_buffer_t *buf, long count)
{
    if (count) {
        blip_buffer_remove_silence(buf, count);
        long remain = blip_buffer_samples_avail(buf) + BLIP_BUFFER_EXTRA;
        memmove(buf->buffer, buf->buffer + count, (size_t)remain * sizeof(buf->buffer[0]));
        memset(buf->buffer + remain, 0, (size_t)count * sizeof(buf->buffer[0]));
    }
}

long blip_buffer_count_samples(const blip_buffer_t *buf, blip_time_t duration)
{
    unsigned long last_sample = blip_buffer_resampled_time(buf, duration) >> BLIP_BUFFER_ACCURACY;
    unsigned long first_sample = buf->offset >> BLIP_BUFFER_ACCURACY;
    return (long)(last_sample - first_sample);
}

blip_time_t blip_buffer_count_clocks(const blip_buffer_t *buf, long count)
{
    if (!buf->factor) {
        assert(0);
        return 0;
    }
    if (count > buf->buffer_size) {
        count = buf->buffer_size;
    }
    blip_resampled_time_t time = (blip_resampled_time_t)count << BLIP_BUFFER_ACCURACY;
    return (blip_time_t)((time - buf->offset + buf->factor - 1) / buf->factor);
}

long blip_buffer_read_samples(blip_buffer_t *buf, blip_sample_t *out, long max_samples)
{
    long count = blip_buffer_samples_avail(buf);
    if (count > max_samples) {
        count = max_samples;
    }

    if (count) {
        const blip_long *reader_buf = buf->buffer;
        blip_long reader_accum = buf->reader_accum;
        const int bass = buf->bass_shift;

        for (long n = count; n; --n) {
            blip_long s = reader_accum >> (BLIP_SAMPLE_BITS - 16);
            if ((int16_t)s != s) {
                s = 0x7FFF - (s >> 24);
            }
            *out++ = (blip_sample_t)s;
            reader_accum += *reader_buf++ - (reader_accum >> bass);
        }

        buf->reader_accum = reader_accum;
        blip_buffer_remove_samples(buf, count);
    }
    return count;
}

long blip_buffer_read_samples_interleaved(blip_buffer_t *buf, blip_sample_t *out, long max_frames, int channels)
{
    long count = blip_buffer_samples_avail(buf);
    if (count > max_frames) {
        count = max_frames;
    }

    if (count) {
        const blip_long *reader_buf = buf->buffer;
        blip_long reader_accum = buf->reader_accum;
        const int bass = buf->bass_shift;

        if (channels == 1) {
            for (long n = count; n; --n) {
                blip_long s = reader_accum >> (BLIP_SAMPLE_BITS - 16);
                if ((int16_t)s != s) {
                    s = 0x7FFF - (s >> 24);
                }
                *out++ = (blip_sample_t)s;
                reader_accum += *reader_buf++ - (reader_accum >> bass);
            }
        } else if ((((uintptr_t)out) & (sizeof(uint32_t) - 1)) == 0) {
            /* Stereo is mono duplicated to L/R. If aligned, one packed store is
               faster and still byte-identical on little-endian targets. */
            uint32_t *out32 = (uint32_t *)out;
            for (long n = count; n; --n) {
                blip_long s = reader_accum >> (BLIP_SAMPLE_BITS - 16);
                if ((int16_t)s != s) {
                    s = 0x7FFF - (s >> 24);
                }
                uint32_t packed = (uint16_t)s;
                *out32++ = packed | (packed << 16);
                reader_accum += *reader_buf++ - (reader_accum >> bass);
            }
        } else {
            for (long n = count; n; --n) {
                blip_long s = reader_accum >> (BLIP_SAMPLE_BITS - 16);
                if ((int16_t)s != s) {
                    s = 0x7FFF - (s >> 24);
                }
                out[0] = (blip_sample_t)s;
                out[1] = (blip_sample_t)s;
                out += 2;
                reader_accum += *reader_buf++ - (reader_accum >> bass);
            }
        }

        buf->reader_accum = reader_accum;
        blip_buffer_remove_samples(buf, count);
    }
    return count;
}

void blip_buffer_set_modified(blip_buffer_t *buf)
{
    buf->modified = 1;
}

int blip_buffer_clear_modified(blip_buffer_t *buf)
{
    int modified = buf->modified;
    buf->modified = 0;
    return modified;
}

blip_resampled_time_t blip_buffer_resampled_duration(const blip_buffer_t *buf, int time)
{
    return (blip_resampled_time_t)time * buf->factor;
}

blip_resampled_time_t blip_buffer_resampled_time(const blip_buffer_t *buf, blip_time_t time)
{
    return (blip_resampled_time_t)time * buf->factor + buf->offset;
}

static void gen_sinc(float *out, int count, double oversample, double treble, double cutoff)
{
    if (cutoff >= 0.999) {
        cutoff = 0.999;
    }
    if (treble < -300.0) {
        treble = -300.0;
    }
    if (treble > 5.0) {
        treble = 5.0;
    }

    const double maxh = 4096.0;
    const double rolloff = pow(10.0, 1.0 / (maxh * 20.0) * treble / (1.0 - cutoff));
    const double pow_a_n = pow(rolloff, maxh - maxh * cutoff);
    const double to_angle = M_PI / 2 / maxh / oversample;

    for (int i = 0; i < count; i++) {
        double angle = ((i - count) * 2 + 1) * to_angle;
        double angle_maxh = angle * maxh;
        double angle_maxh_mid = angle_maxh * cutoff;
        double y = maxh;

        if (angle_maxh_mid) {
            y *= sin(angle_maxh_mid) / angle_maxh_mid;
        }

        double cosa = cos(angle);
        double den = 1 + rolloff * (rolloff - cosa - cosa);
        if (den > 1e-13) {
            double num =
                (cos(angle_maxh - angle) * rolloff - cos(angle_maxh)) * pow_a_n -
                cos(angle_maxh_mid - angle) * rolloff + cos(angle_maxh_mid);
            y = y * cutoff + num / den;
        }
        out[i] = (float)y;
    }
}

static void blip_eq_generate(const blip_eq_t *eq, float *out, int count)
{
    double oversample = BLIP_RES * 2.25 / count + 0.85;
    double half_rate = eq->sample_rate * 0.5;
    if (eq->cutoff_freq) {
        oversample = half_rate / eq->cutoff_freq;
    }
    double cutoff = eq->rolloff_freq * oversample / half_rate;

    gen_sinc(out, count, (double)BLIP_RES * oversample, eq->treble, cutoff);

    double to_fraction = M_PI / (count - 1);
    for (int i = count; i--;) {
        out[i] *= (float)(0.54 - 0.46 * cos(i * to_fraction));
    }
}

void blip_synth_init(blip_synth_t *synth, int quality, int range)
{
    memset(synth, 0, sizeof(*synth));
    synth->quality = quality;
    synth->width = quality;
    synth->range = range ? range : 1;
    synth->impulse_count = BLIP_RES / 2 * synth->width + 1;
    synth->initialized = 1;
}

static void blip_synth_adjust_impulse(blip_synth_t *synth)
{
    int size = synth->impulse_count;
    for (int p = BLIP_RES; p-- >= BLIP_RES / 2;) {
        int p2 = BLIP_RES - 2 - p;
        long error = synth->kernel_unit;
        for (int i = 1; i < size; i += BLIP_RES) {
            error -= synth->impulses[i + p];
            error -= synth->impulses[i + p2];
        }
        if (p == p2) {
            error /= 2;
        }
        synth->impulses[size - BLIP_RES + p] = (int16_t)(synth->impulses[size - BLIP_RES + p] + error);
    }
}

void blip_synth_treble_eq(blip_synth_t *synth, const blip_eq_t *eq)
{
    float fimpulse[BLIP_RES / 2 * (BLIP_WIDEST_IMPULSE - 1) + BLIP_RES * 2];
    int half_size = BLIP_RES / 2 * (synth->width - 1);
    blip_eq_generate(eq, &fimpulse[BLIP_RES], half_size);

    /* Generate only half the impulse and mirror it; the correction pass below
       preserves exact DC gain for each fractional phase. */
    for (int i = BLIP_RES; i--;) {
        fimpulse[BLIP_RES + half_size + i] = fimpulse[BLIP_RES + half_size - 1 - i];
    }
    for (int i = 0; i < BLIP_RES; i++) {
        fimpulse[i] = 0.0f;
    }

    double total = 0.0;
    for (int i = 0; i < half_size; i++) {
        total += fimpulse[BLIP_RES + i];
    }

    const double base_unit = 32768.0;
    double rescale = base_unit / 2 / total;
    synth->kernel_unit = (long)base_unit;

    double sum = 0.0;
    double next = 0.0;
    int impulses_size = synth->impulse_count;
    for (int i = 0; i < impulses_size; i++) {
        synth->impulses[i] = (int16_t)floor((next - sum) * rescale + 0.5);
        sum += fimpulse[i];
        next += fimpulse[i + BLIP_RES];
    }
    blip_synth_adjust_impulse(synth);

    double vol = synth->volume_unit;
    if (vol) {
        synth->volume_unit = 0.0;
        blip_synth_volume(synth, vol * synth->range);
    }
}

void blip_synth_volume(blip_synth_t *synth, double volume)
{
    double new_unit = volume * (1.0 / (synth->range < 0 ? -synth->range : synth->range));
    if (new_unit == synth->volume_unit) {
        return;
    }

    if (!synth->kernel_unit) {
        blip_eq_t eq;
        blip_eq_init(&eq, -8.0);
        blip_synth_treble_eq(synth, &eq);
    }

    synth->volume_unit = new_unit;
    double factor = new_unit * (1L << BLIP_SAMPLE_BITS) / synth->kernel_unit;
    if (factor > 0.0) {
        int shift = 0;
        while (factor < 2.0) {
            shift++;
            factor *= 2.0;
        }
        if (shift) {
            synth->kernel_unit >>= shift;
            assert(synth->kernel_unit > 0);
            long offset = 0x8000 + (1 << (shift - 1));
            long offset2 = 0x8000 >> shift;
            for (int i = synth->impulse_count; i--;) {
                synth->impulses[i] = (int16_t)(((synth->impulses[i] + offset) >> shift) - offset2);
            }
            blip_synth_adjust_impulse(synth);
        }
    }
    synth->delta_factor = (int)floor(factor + 0.5);
}

void blip_synth_offset_resampled(blip_synth_t *synth, blip_resampled_time_t time, int delta, blip_buffer_t *buf)
{
    assert((blip_long)(time >> BLIP_BUFFER_ACCURACY) < buf->buffer_size);
    delta *= synth->delta_factor;
    blip_long *b = buf->buffer + (time >> BLIP_BUFFER_ACCURACY);
    int phase = (int)(time >> (BLIP_BUFFER_ACCURACY - BLIP_PHASE_BITS) & (BLIP_RES - 1));

    const int16_t *imp = synth->impulses + BLIP_RES - phase;

#define ADD_IMP(out_index, in_index) \
    b[(out_index)] += (blip_long)imp[BLIP_RES * (in_index)] * delta

    if (synth->quality == BLIP_GOOD_QUALITY) {
        /* The NES port only uses quality 12 for squares and quality 8 for TND.
           Specializing both avoids per-transition loops and matches templates. */
        ADD_IMP(2, 0);
        ADD_IMP(3, 1);
        ADD_IMP(4, 2);
        ADD_IMP(5, 3);
        ADD_IMP(6, 4);
        ADD_IMP(7, 5);
        imp = synth->impulses + phase;
        ADD_IMP(8, 5);
        ADD_IMP(9, 4);
        ADD_IMP(10, 3);
        ADD_IMP(11, 2);
        ADD_IMP(12, 1);
        ADD_IMP(13, 0);
    } else {
        ADD_IMP(4, 0);
        ADD_IMP(5, 1);
        ADD_IMP(6, 2);
        ADD_IMP(7, 3);
        imp = synth->impulses + phase;
        ADD_IMP(8, 3);
        ADD_IMP(9, 2);
        ADD_IMP(10, 1);
        ADD_IMP(11, 0);
    }

#undef ADD_IMP
}

void blip_synth_offset(blip_synth_t *synth, blip_time_t time, int delta, blip_buffer_t *buf)
{
    blip_synth_offset_resampled(synth, (blip_resampled_time_t)time * buf->factor + buf->offset, delta, buf);
}

void blip_synth_offset_inline(blip_synth_t *synth, blip_time_t time, int delta, blip_buffer_t *buf)
{
    blip_synth_offset_resampled(synth, (blip_resampled_time_t)time * buf->factor + buf->offset, delta, buf);
}
