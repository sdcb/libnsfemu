#include "nes_apu.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

enum {
    AMP_RANGE = 15,
    SQUARE_NEGATE_FLAG = 0x08,
    SQUARE_SHIFT_MASK = 0x07,
    SQUARE_PHASE_RANGE = 8,
    TRIANGLE_PHASE_RANGE = 16,
    DMC_LOOP_FLAG = 0x40
};

static void osc_clock_length(struct nes_osc *osc, int halt_mask)
{
    if (osc->length_counter && !(osc->regs[0] & halt_mask)) {
        osc->length_counter--;
    }
}

static int osc_period(const struct nes_osc *osc)
{
    return (osc->regs[3] & 7) * 0x100 + (osc->regs[2] & 0xFF);
}

static void osc_reset(struct nes_osc *osc)
{
    osc->delay = 0;
    osc->last_amp = 0;
}

static int osc_update_amp(struct nes_osc *osc, int amp)
{
    int delta = amp - osc->last_amp;
    osc->last_amp = amp;
    return delta;
}

static void envelope_reset(struct nes_envelope *env)
{
    env->envelope = 0;
    env->env_delay = 0;
    osc_reset(&env->osc);
}

static void envelope_clock(struct nes_envelope *env)
{
    int period = env->osc.regs[0] & 15;
    if (env->osc.reg_written[3]) {
        env->osc.reg_written[3] = false;
        env->env_delay = period;
        env->envelope = 15;
    } else if (--env->env_delay < 0) {
        env->env_delay = period;
        if (env->envelope | (env->osc.regs[0] & 0x20)) {
            env->envelope = (env->envelope - 1) & 15;
        }
    }
}

static int envelope_volume(const struct nes_envelope *env)
{
    if (env->osc.length_counter == 0) {
        return 0;
    }
    return (env->osc.regs[0] & 0x10) ? (env->osc.regs[0] & 15) : env->envelope;
}

static void square_reset(struct nes_square *square)
{
    square->sweep_delay = 0;
    envelope_reset(&square->env);
}

static void triangle_reset(struct nes_triangle *triangle)
{
    triangle->linear_counter = 0;
    triangle->phase = 1;
    osc_reset(&triangle->osc);
}

static void noise_reset(struct nes_noise *noise)
{
    noise->noise = 1 << 14;
    envelope_reset(&noise->env);
}

static void apu_irq_changed(nes_apu_t *apu);

static nes_time_t dmc_next_read_time(const struct nes_dmc *dmc)
{
    if (dmc->osc.length_counter == 0) {
        return NES_APU_NO_IRQ;
    }
    return dmc->apu->last_dmc_time + dmc->osc.delay + (long)(dmc->bits_remain - 1) * dmc->period;
}

static void dmc_recalc_irq(struct nes_dmc *dmc)
{
    nes_time_t irq = NES_APU_NO_IRQ;
    if (dmc->irq_enabled && dmc->osc.length_counter) {
        /* DMC IRQ is predictable from remaining bytes and bit timing. */
        irq = dmc->apu->last_dmc_time + dmc->osc.delay +
              ((dmc->osc.length_counter - 1) * 8 + dmc->bits_remain - 1) * (nes_time_t)dmc->period + 1;
    }
    if (irq != dmc->next_irq) {
        dmc->next_irq = irq;
        apu_irq_changed(dmc->apu);
    }
}

static void dmc_reload_sample(struct nes_dmc *dmc)
{
    dmc->address = 0x4000 + dmc->osc.regs[2] * 0x40;
    dmc->osc.length_counter = dmc->osc.regs[3] * 0x10 + 1;
}

static void dmc_fill_buffer(struct nes_dmc *dmc)
{
    if (!dmc->buf_full && dmc->osc.length_counter) {
        /* DMC performs CPU-visible memory reads through the NSF memory map. */
        assert(dmc->prg_reader);
        dmc->buf = dmc->prg_reader(dmc->prg_reader_data, 0x8000u + dmc->address);
        dmc->address = (dmc->address + 1) & 0x7FFF;
        dmc->buf_full = true;
        if (--dmc->osc.length_counter == 0) {
            if (dmc->osc.regs[0] & DMC_LOOP_FLAG) {
                /* Looping samples immediately reload without disabling DMC. */
                dmc_reload_sample(dmc);
            } else {
                /* Non-looping DMC clears enable and may raise IRQ. */
                dmc->apu->osc_enables &= ~0x10;
                dmc->irq_flag = dmc->irq_enabled;
                dmc->next_irq = NES_APU_NO_IRQ;
                apu_irq_changed(dmc->apu);
            }
        }
    }
}

static void dmc_start(struct nes_dmc *dmc)
{
    dmc_reload_sample(dmc);
    dmc_fill_buffer(dmc);
    dmc_recalc_irq(dmc);
}

static void dmc_reset(struct nes_dmc *dmc)
{
    dmc->address = 0;
    dmc->dac = 0;
    dmc->buf = 0;
    dmc->bits_remain = 1;
    dmc->bits = 0;
    dmc->buf_full = false;
    dmc->silence = true;
    dmc->next_irq = NES_APU_NO_IRQ;
    dmc->irq_flag = false;
    dmc->irq_enabled = false;
    osc_reset(&dmc->osc);
    dmc->period = 0x1AC;
}

static const int16_t dmc_period_table[2][16] = {
    {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54},
    {398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50}
};

static const uint8_t dac_table[128] = {
     0, 1, 2, 3, 4, 5, 6, 7, 7, 8, 9,10,11,12,13,14,
    15,15,16,17,18,19,20,20,21,22,23,24,24,25,26,27,
    27,28,29,30,31,31,32,33,33,34,35,36,36,37,38,38,
    39,40,41,41,42,43,43,44,45,45,46,47,47,48,48,49,
    50,50,51,52,52,53,53,54,55,55,56,56,57,58,58,59,
    59,60,60,61,61,62,63,63,64,64,65,65,66,66,67,67,
    68,68,69,70,70,71,71,72,72,73,73,74,74,75,75,75,
    76,76,77,77,78,78,79,79,80,80,81,81,82,82,82,83,
};

static void dmc_write_register(struct nes_dmc *dmc, int addr, int data)
{
    if (addr == 0) {
        dmc->period = dmc_period_table[dmc->pal_mode ? 1 : 0][data & 15];
        dmc->irq_enabled = (data & 0xC0) == 0x80;
        dmc->irq_flag &= dmc->irq_enabled;
        dmc_recalc_irq(dmc);
    } else if (addr == 1) {
        int old_dac = dmc->dac;
        dmc->dac = data & 0x7F;
        int faked_nonlinear = dmc->dac - (dac_table[dmc->dac] - dac_table[old_dac]);
        if (!dmc->nonlinear) {
            dmc->osc.last_amp = faked_nonlinear;
        }
    }
}

static void dmc_run(struct nes_dmc *dmc, nes_time_t time, nes_time_t end_time)
{
    int delta = osc_update_amp(&dmc->osc, dmc->dac);
    if (!dmc->osc.output) {
        /* Muting skips audio transitions but still advances DMC timing state. */
        dmc->silence = true;
    } else {
        blip_buffer_set_modified(dmc->osc.output);
        if (delta) {
            blip_synth_offset(&dmc->synth, time, delta, dmc->osc.output);
        }
    }

    time += dmc->osc.delay;
    if (time < end_time) {
        int bits_remain = dmc->bits_remain;
        if (dmc->silence && !dmc->buf_full) {
            /* Fast-forward silent empty-buffer periods by whole DMC bit clocks. */
            int count = (end_time - time + dmc->period - 1) / dmc->period;
            bits_remain = (bits_remain - 1 + 8 - (count % 8)) % 8 + 1;
            time += count * dmc->period;
        } else {
            blip_buffer_t *output = dmc->osc.output;
            const int period = dmc->period;
            int bits = dmc->bits;
            int dac = dmc->dac;

            do {
                if (!dmc->silence) {
                    int step = (bits & 1) * 4 - 2;
                    bits >>= 1;
                    if ((unsigned)(dac + step) <= 0x7F) {
                        dac += step;
                        if (output) {
                            blip_synth_offset_inline(&dmc->synth, time, step, output);
                        }
                    }
                }

                time += period;

                if (--bits_remain == 0) {
                    bits_remain = 8;
                    if (!dmc->buf_full) {
                        dmc->silence = true;
                    } else {
                        /* A full byte shifts out LSB-first after the previous
                           byte's last bit has completed. */
                        dmc->silence = false;
                        bits = dmc->buf;
                        dmc->buf_full = false;
                        if (!output) {
                            dmc->silence = true;
                        }
                        dmc_fill_buffer(dmc);
                    }
                }
            } while (time < end_time);

            dmc->dac = dac;
            dmc->osc.last_amp = dac;
            dmc->bits = bits;
        }
        dmc->bits_remain = bits_remain;
    }
    dmc->osc.delay = time - end_time;
}

static void square_clock_sweep(struct nes_square *square, int negative_adjust)
{
    int sweep = square->env.osc.regs[1];
    if (--square->sweep_delay < 0) {
        square->env.osc.reg_written[1] = true;
        int period = osc_period(&square->env.osc);
        int shift = sweep & SQUARE_SHIFT_MASK;
        if (shift && (sweep & 0x80) && period >= 8) {
            int offset = period >> shift;
            if (sweep & SQUARE_NEGATE_FLAG) {
                offset = negative_adjust - offset;
            }
            if (period + offset < 0x800) {
                period += offset;
                square->env.osc.regs[2] = period & 0xFF;
                square->env.osc.regs[3] = (square->env.osc.regs[3] & ~7) | ((period >> 8) & 7);
            }
        }
    }
    if (square->env.osc.reg_written[1]) {
        square->env.osc.reg_written[1] = false;
        square->sweep_delay = (sweep >> 4) & 7;
    }
}

static nes_time_t square_maintain_phase(struct nes_square *square, nes_time_t time, nes_time_t end_time, nes_time_t timer_period)
{
    nes_time_t remain = end_time - time;
    if (remain > 0) {
        int count = (remain + timer_period - 1) / timer_period;
        square->phase = (square->phase + count) & (SQUARE_PHASE_RANGE - 1);
        time += (int32_t)count * timer_period;
    }
    return time;
}

static void square_run(struct nes_square *square, nes_time_t time, nes_time_t end_time)
{
    const int period = osc_period(&square->env.osc);
    const int timer_period = (period + 1) * 2;

    if (!square->env.osc.output) {
        /* A muted square must keep phase so unmuting is click-compatible. */
        square->env.osc.delay = square_maintain_phase(square, time + square->env.osc.delay, end_time, timer_period) - end_time;
        return;
    }

    blip_buffer_set_modified(square->env.osc.output);

    int offset = period >> (square->env.osc.regs[1] & SQUARE_SHIFT_MASK);
    if (square->env.osc.regs[1] & SQUARE_NEGATE_FLAG) {
        offset = 0;
    }

    const int volume = envelope_volume(&square->env);
    if (volume == 0 || period < 8 || (period + offset) >= 0x800) {
        /* Hardware silences inaudible/invalid sweep states but phase continues. */
        if (square->env.osc.last_amp) {
            blip_synth_offset(square->synth, time, -square->env.osc.last_amp, square->env.osc.output);
            square->env.osc.last_amp = 0;
        }
        time += square->env.osc.delay;
        time = square_maintain_phase(square, time, end_time, timer_period);
    } else {
        int duty_select = (square->env.osc.regs[0] >> 6) & 3;
        int duty = 1 << duty_select;
        int amp = 0;
        if (duty_select == 3) {
            duty = 2;
            amp = volume;
        }
        if (square->phase < duty) {
            amp ^= volume;
        }

        int delta = osc_update_amp(&square->env.osc, amp);
        if (delta) {
            blip_synth_offset(square->synth, time, delta, square->env.osc.output);
        }

        time += square->env.osc.delay;
        if (time < end_time) {
            blip_buffer_t *output = square->env.osc.output;
            int local_delta = amp * 2 - volume;
            int phase = square->phase;
            do {
                phase = (phase + 1) & (SQUARE_PHASE_RANGE - 1);
                if (phase == 0 || phase == duty) {
                    local_delta = -local_delta;
                    blip_synth_offset_inline(square->synth, time, local_delta, output);
                }
                time += timer_period;
            } while (time < end_time);

            square->env.osc.last_amp = (local_delta + volume) >> 1;
            square->phase = phase;
        }
    }
    square->env.osc.delay = time - end_time;
}

static void triangle_clock_linear_counter(struct nes_triangle *triangle)
{
    if (triangle->osc.reg_written[3]) {
        triangle->linear_counter = triangle->osc.regs[0] & 0x7F;
    } else if (triangle->linear_counter) {
        triangle->linear_counter--;
    }
    if (!(triangle->osc.regs[0] & 0x80)) {
        triangle->osc.reg_written[3] = false;
    }
}

static int triangle_calc_amp(const struct nes_triangle *triangle)
{
    int amp = TRIANGLE_PHASE_RANGE - triangle->phase;
    if (amp < 0) {
        amp = triangle->phase - (TRIANGLE_PHASE_RANGE + 1);
    }
    return amp;
}

static nes_time_t triangle_maintain_phase(struct nes_triangle *triangle, nes_time_t time, nes_time_t end_time, nes_time_t timer_period)
{
    nes_time_t remain = end_time - time;
    if (remain > 0) {
        int count = (remain + timer_period - 1) / timer_period;
        triangle->phase = ((unsigned)triangle->phase + 1 - count) & (TRIANGLE_PHASE_RANGE * 2 - 1);
        triangle->phase++;
        time += (int32_t)count * timer_period;
    }
    return time;
}

static void triangle_run(struct nes_triangle *triangle, nes_time_t time, nes_time_t end_time)
{
    const int timer_period = osc_period(&triangle->osc) + 1;
    if (!triangle->osc.output) {
        /* Muted triangle keeps its sequencer only when counters allow it to run. */
        time += triangle->osc.delay;
        triangle->osc.delay = 0;
        if (triangle->osc.length_counter && triangle->linear_counter && timer_period >= 3) {
            triangle->osc.delay = triangle_maintain_phase(triangle, time, end_time, timer_period) - end_time;
        }
        return;
    }

    blip_buffer_set_modified(triangle->osc.output);

    int delta = osc_update_amp(&triangle->osc, triangle_calc_amp(triangle));
    if (delta) {
        blip_synth_offset(&triangle->synth, time, delta, triangle->osc.output);
    }

    time += triangle->osc.delay;
    if (triangle->osc.length_counter == 0 || triangle->linear_counter == 0 || timer_period < 3) {
        /* Triangle does not emit steps while either counter is zero. */
        time = end_time;
    } else if (time < end_time) {
        blip_buffer_t *output = triangle->osc.output;
        int phase = triangle->phase;
        int volume = 1;
        if (phase > TRIANGLE_PHASE_RANGE) {
            phase -= TRIANGLE_PHASE_RANGE;
            volume = -volume;
        }

        do {
            if (--phase == 0) {
                phase = TRIANGLE_PHASE_RANGE;
                volume = -volume;
            } else {
                blip_synth_offset_inline(&triangle->synth, time, volume, output);
            }
            time += timer_period;
        } while (time < end_time);

        if (volume < 0) {
            phase += TRIANGLE_PHASE_RANGE;
        }
        triangle->phase = phase;
        triangle->osc.last_amp = triangle_calc_amp(triangle);
    }
    triangle->osc.delay = time - end_time;
}

static const int16_t noise_period_table[16] = {
    0x004, 0x008, 0x010, 0x020, 0x040, 0x060, 0x080, 0x0A0,
    0x0CA, 0x0FE, 0x17C, 0x1FC, 0x2FA, 0x3F8, 0x7F2, 0xFE4
};

static void noise_run(struct nes_noise *noise, nes_time_t time, nes_time_t end_time)
{
    int period = noise_period_table[noise->env.osc.regs[2] & 15];

    if (!noise->env.osc.output) {
        time += noise->env.osc.delay;
        noise->env.osc.delay = time + (end_time - time + period - 1) / period * period - end_time;
        return;
    }

    blip_buffer_set_modified(noise->env.osc.output);

    const int volume = envelope_volume(&noise->env);
    int amp = (noise->noise & 1) ? volume : 0;
    int delta = osc_update_amp(&noise->env.osc, amp);
    if (delta) {
        blip_synth_offset(&noise->synth, time, delta, noise->env.osc.output);
    }

    time += noise->env.osc.delay;
    if (time < end_time) {
        const int mode_flag = 0x80;
        if (!volume) {
            /* When muted by envelope, approximate LFSR progress cheaply; this is
               the same tradeoff libgme makes for the inaudible path. */
            time += (end_time - time + period - 1) / period * period;
            if (!(noise->env.osc.regs[2] & mode_flag)) {
                int feedback = (noise->noise << 13) ^ (noise->noise << 14);
                noise->noise = (feedback & 0x4000) | (noise->noise >> 1);
            }
        } else {
            blip_buffer_t *output = noise->env.osc.output;
            blip_resampled_time_t rperiod = blip_buffer_resampled_duration(output, period);
            blip_resampled_time_t rtime = blip_buffer_resampled_time(output, time);
            int shift_reg = noise->noise;
            int local_delta = amp * 2 - volume;
            const int tap = (noise->env.osc.regs[2] & mode_flag) ? 8 : 13;
            do {
                int feedback = (shift_reg << tap) ^ (shift_reg << 14);
                time += period;
                if ((shift_reg + 1) & 2) {
                    local_delta = -local_delta;
                    blip_synth_offset_resampled(&noise->synth, rtime, local_delta, output);
                }
                rtime += rperiod;
                shift_reg = (feedback & 0x4000) | (shift_reg >> 1);
            } while (time < end_time);

            noise->env.osc.last_amp = (local_delta + volume) >> 1;
            noise->noise = shift_reg;
        }
    }
    noise->env.osc.delay = time - end_time;
}

void nes_apu_init(nes_apu_t *apu)
{
    memset(apu, 0, sizeof(*apu));
    blip_synth_init(&apu->square_synth, BLIP_GOOD_QUALITY, 1);
    blip_synth_init(&apu->triangle.synth, BLIP_MED_QUALITY, 1);
    blip_synth_init(&apu->noise.synth, BLIP_MED_QUALITY, 1);
    blip_synth_init(&apu->dmc.synth, BLIP_MED_QUALITY, 1);

    apu->square1.synth = &apu->square_synth;
    apu->square2.synth = &apu->square_synth;
    apu->dmc.apu = apu;
    apu->tempo = 1.0;

    apu->oscs[0] = &apu->square1.env.osc;
    apu->oscs[1] = &apu->square2.env.osc;
    apu->oscs[2] = &apu->triangle.osc;
    apu->oscs[3] = &apu->noise.env.osc;
    apu->oscs[4] = &apu->dmc.osc;

    nes_apu_output(apu, NULL);
    nes_apu_volume(apu, 1.0);
    nes_apu_reset(apu, false, 0);
}

void nes_apu_output(nes_apu_t *apu, blip_buffer_t *buffer)
{
    for (int i = 0; i < NES_APU_OSC_COUNT; i++) {
        nes_apu_osc_output(apu, i, buffer);
    }
}

void nes_apu_osc_output(nes_apu_t *apu, int index, blip_buffer_t *buffer)
{
    assert((unsigned)index < NES_APU_OSC_COUNT);
    apu->oscs[index]->output = buffer;
}

void nes_apu_dmc_reader(nes_apu_t *apu, int (*reader)(void *, nes_addr_t), void *user_data)
{
    apu->dmc.prg_reader = reader;
    apu->dmc.prg_reader_data = user_data;
}

void nes_apu_treble_eq(nes_apu_t *apu, const blip_eq_t *eq)
{
    blip_synth_treble_eq(&apu->square_synth, eq);
    blip_synth_treble_eq(&apu->triangle.synth, eq);
    blip_synth_treble_eq(&apu->noise.synth, eq);
    blip_synth_treble_eq(&apu->dmc.synth, eq);
}

void nes_apu_volume(nes_apu_t *apu, double volume)
{
    apu->dmc.nonlinear = false;
    blip_synth_volume(&apu->square_synth, 0.1128 / AMP_RANGE * volume);
    blip_synth_volume(&apu->triangle.synth, 0.12765 / AMP_RANGE * volume);
    blip_synth_volume(&apu->noise.synth, 0.0741 / AMP_RANGE * volume);
    blip_synth_volume(&apu->dmc.synth, 0.42545 / 127 * volume);
}

void nes_apu_set_tempo(nes_apu_t *apu, double tempo)
{
    apu->tempo = tempo;
    apu->frame_period = apu->dmc.pal_mode ? 8314 : 7458;
    if (tempo != 1.0) {
        apu->frame_period = ((int)(apu->frame_period / tempo)) & ~1;
    }
}

void nes_apu_reset(nes_apu_t *apu, bool pal_mode, int initial_dmc_dac)
{
    apu->dmc.pal_mode = pal_mode;
    nes_apu_set_tempo(apu, apu->tempo);

    square_reset(&apu->square1);
    square_reset(&apu->square2);
    triangle_reset(&apu->triangle);
    noise_reset(&apu->noise);
    dmc_reset(&apu->dmc);

    apu->last_time = 0;
    apu->last_dmc_time = 0;
    apu->osc_enables = 0;
    apu->irq_flag = false;
    apu->earliest_irq = NES_APU_NO_IRQ;
    apu->frame_delay = 1;
    nes_apu_write_register(apu, 0, 0x4017, 0x00);
    nes_apu_write_register(apu, 0, 0x4015, 0x00);
    for (nes_addr_t addr = NES_APU_START_ADDR; addr <= 0x4013; addr++) {
        nes_apu_write_register(apu, 0, addr, (addr & 3) ? 0x00 : 0x10);
    }

    apu->dmc.dac = initial_dmc_dac;
    if (!apu->dmc.nonlinear) {
        apu->triangle.osc.last_amp = 15;
        apu->dmc.osc.last_amp = initial_dmc_dac;
    }
}

static void apu_irq_changed(nes_apu_t *apu)
{
    nes_time_t new_irq = apu->dmc.next_irq;
    if (apu->dmc.irq_flag | apu->irq_flag) {
        new_irq = 0;
    } else if (new_irq > apu->next_irq) {
        new_irq = apu->next_irq;
    }
    apu->earliest_irq = new_irq;
}

void nes_apu_run_until(nes_apu_t *apu, nes_time_t end_time)
{
    assert(end_time >= apu->last_dmc_time);
    if (end_time > dmc_next_read_time(&apu->dmc)) {
        nes_time_t start = apu->last_dmc_time;
        apu->last_dmc_time = end_time;
        dmc_run(&apu->dmc, start, end_time);
    }
}

static void apu_run_until_full(nes_apu_t *apu, nes_time_t end_time)
{
    assert(end_time >= apu->last_time);
    if (end_time == apu->last_time) {
        return;
    }

    if (apu->last_dmc_time < end_time) {
        nes_time_t start = apu->last_dmc_time;
        apu->last_dmc_time = end_time;
        dmc_run(&apu->dmc, start, end_time);
    }

    for (;;) {
        nes_time_t time = apu->last_time + apu->frame_delay;
        if (time > end_time) {
            time = end_time;
        }
        apu->frame_delay -= time - apu->last_time;

        square_run(&apu->square1, apu->last_time, time);
        square_run(&apu->square2, apu->last_time, time);
        triangle_run(&apu->triangle, apu->last_time, time);
        noise_run(&apu->noise, apu->last_time, time);
        apu->last_time = time;

        if (time == end_time) {
            break;
        }

        apu->frame_delay = apu->frame_period;
        switch (apu->frame++) {
        case 0:
            if (!(apu->frame_mode & 0xC0)) {
                /* Four-step mode can raise a frame IRQ on this sequence. */
                apu->next_irq = time + apu->frame_period * 4 + 2;
                apu->irq_flag = true;
            }
            /* fall through */
        case 2:
            /* Length counters and square sweeps clock only on half-frame steps. */
            osc_clock_length(&apu->square1.env.osc, 0x20);
            osc_clock_length(&apu->square2.env.osc, 0x20);
            osc_clock_length(&apu->noise.env.osc, 0x20);
            osc_clock_length(&apu->triangle.osc, 0x80);
            square_clock_sweep(&apu->square1, -1);
            square_clock_sweep(&apu->square2, 0);
            if (apu->dmc.pal_mode && apu->frame == 3) {
                apu->frame_delay -= 2;
            }
            break;
        case 1:
            if (!apu->dmc.pal_mode) {
                apu->frame_delay -= 2;
            }
            break;
        case 3:
            apu->frame = 0;
            if (apu->frame_mode & 0x80) {
                apu->frame_delay += apu->frame_period - (apu->dmc.pal_mode ? 2 : 6);
            }
            break;
        default:
            break;
        }

        triangle_clock_linear_counter(&apu->triangle);
        envelope_clock(&apu->square1.env);
        envelope_clock(&apu->square2.env);
        envelope_clock(&apu->noise.env);
    }
}

void nes_apu_end_frame(nes_apu_t *apu, nes_time_t end_time)
{
    if (end_time > apu->last_time) {
        apu_run_until_full(apu, end_time);
    }

    apu->last_time -= end_time;
    assert(apu->last_time >= 0);
    apu->last_dmc_time -= end_time;
    assert(apu->last_dmc_time >= 0);

    if (apu->next_irq != NES_APU_NO_IRQ) {
        apu->next_irq -= end_time;
        if (apu->next_irq < 0) {
            apu->next_irq = 0;
        }
    }
    if (apu->dmc.next_irq != NES_APU_NO_IRQ) {
        apu->dmc.next_irq -= end_time;
        if (apu->dmc.next_irq < 0) {
            apu->dmc.next_irq = 0;
        }
    }
    if (apu->earliest_irq != NES_APU_NO_IRQ) {
        apu->earliest_irq -= end_time;
        if (apu->earliest_irq < 0) {
            apu->earliest_irq = 0;
        }
    }
}

static const uint8_t length_table[0x20] = {
    0x0A, 0xFE, 0x14, 0x02, 0x28, 0x04, 0x50, 0x06,
    0xA0, 0x08, 0x3C, 0x0A, 0x0E, 0x0C, 0x1A, 0x0E,
    0x0C, 0x10, 0x18, 0x12, 0x30, 0x14, 0x60, 0x16,
    0xC0, 0x18, 0x48, 0x1A, 0x10, 0x1C, 0x20, 0x1E
};

void nes_apu_write_register(nes_apu_t *apu, nes_time_t time, nes_addr_t addr, int data)
{
    if ((unsigned)(addr - NES_APU_START_ADDR) > NES_APU_END_ADDR - NES_APU_START_ADDR) {
        return;
    }

    /* Register effects are timed, so synthesize all channels up to the CPU write
       cycle before mutating any oscillator state. */
    apu_run_until_full(apu, time);

    if (addr < 0x4014) {
        /* $4000-$4013 are per-channel registers in 4-byte groups. */
        int osc_index = (int)((addr - NES_APU_START_ADDR) >> 2);
        struct nes_osc *osc = apu->oscs[osc_index];
        int reg = addr & 3;
        osc->regs[reg] = (uint8_t)data;
        osc->reg_written[reg] = true;

        if (osc_index == 4) {
            dmc_write_register(&apu->dmc, reg, data);
        } else if (reg == 3) {
            /* The high timer/length write reloads the channel if it is enabled. */
            if ((apu->osc_enables >> osc_index) & 1) {
                osc->length_counter = length_table[(data >> 3) & 0x1F];
            }
            if (osc_index < 2) {
                ((struct nes_square *)(osc_index == 0 ? &apu->square1 : &apu->square2))->phase = SQUARE_PHASE_RANGE - 1;
            }
        }
    } else if (addr == 0x4015) {
        /* $4015 controls channel enables and acknowledges DMC IRQ. */
        for (int i = NES_APU_OSC_COUNT; i--;) {
            if (!((data >> i) & 1)) {
                apu->oscs[i]->length_counter = 0;
            }
        }

        bool recalc_irq = apu->dmc.irq_flag;
        apu->dmc.irq_flag = false;
        int old_enables = apu->osc_enables;
        apu->osc_enables = data;
        if (!(data & 0x10)) {
            apu->dmc.next_irq = NES_APU_NO_IRQ;
            recalc_irq = true;
        } else if (!(old_enables & 0x10)) {
            dmc_start(&apu->dmc);
        }
        if (recalc_irq) {
            apu_irq_changed(apu);
        }
    } else if (addr == 0x4017) {
        /* $4017 selects 4-step/5-step frame sequencing and frame IRQ inhibit. */
        apu->frame_mode = data;
        bool irq_enabled = !(data & 0x40);
        apu->irq_flag &= irq_enabled;
        apu->next_irq = NES_APU_NO_IRQ;
        apu->frame_delay = (apu->frame_delay & 1);
        apu->frame = 0;

        if (!(data & 0x80)) {
            apu->frame = 1;
            apu->frame_delay += apu->frame_period;
            if (irq_enabled) {
                apu->next_irq = time + apu->frame_delay + apu->frame_period * 3 + 1;
            }
        }
        apu_irq_changed(apu);
    }
}

int nes_apu_read_status(nes_apu_t *apu, nes_time_t time)
{
    apu_run_until_full(apu, time - 1);
    int result = (apu->dmc.irq_flag << 7) | (apu->irq_flag << 6);
    for (int i = 0; i < NES_APU_OSC_COUNT; i++) {
        if (apu->oscs[i]->length_counter) {
            result |= 1 << i;
        }
    }

    apu_run_until_full(apu, time);
    if (apu->irq_flag) {
        result |= 0x40;
        apu->irq_flag = false;
        apu_irq_changed(apu);
    }
    return result;
}
