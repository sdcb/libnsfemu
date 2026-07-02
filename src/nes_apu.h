#ifndef NSFEMU_NES_APU_H
#define NSFEMU_NES_APU_H

#include "blip_buffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef int32_t nes_time_t;
typedef unsigned nes_addr_t;

enum {
    NES_APU_START_ADDR = 0x4000,
    NES_APU_END_ADDR = 0x4017,
    NES_APU_STATUS_ADDR = 0x4015,
    NES_APU_OSC_COUNT = 5,
    NES_APU_NO_IRQ = INT32_MAX / 2 + 1,
    NES_APU_IRQ_WAITING = 0
};

typedef struct nes_apu nes_apu_t;

void nes_apu_init(nes_apu_t *apu);
void nes_apu_output(nes_apu_t *apu, blip_buffer_t *buffer);
void nes_apu_osc_output(nes_apu_t *apu, int index, blip_buffer_t *buffer);
void nes_apu_dmc_reader(nes_apu_t *apu, int (*reader)(void *user_data, nes_addr_t addr), void *user_data);
void nes_apu_reset(nes_apu_t *apu, bool pal_mode, int initial_dmc_dac);
void nes_apu_set_tempo(nes_apu_t *apu, double tempo);
void nes_apu_volume(nes_apu_t *apu, double volume);
void nes_apu_treble_eq(nes_apu_t *apu, const blip_eq_t *eq);
void nes_apu_write_register(nes_apu_t *apu, nes_time_t time, nes_addr_t addr, int data);
int nes_apu_read_status(nes_apu_t *apu, nes_time_t time);
void nes_apu_run_until(nes_apu_t *apu, nes_time_t end_time);
void nes_apu_end_frame(nes_apu_t *apu, nes_time_t end_time);

struct nes_osc {
    uint8_t regs[4];
    bool reg_written[4];
    blip_buffer_t *output;
    int length_counter;
    int delay;
    int last_amp;
};

struct nes_envelope {
    struct nes_osc osc;
    int envelope;
    int env_delay;
};

struct nes_square {
    struct nes_envelope env;
    int phase;
    int sweep_delay;
    blip_synth_t *synth;
};

struct nes_triangle {
    struct nes_osc osc;
    int phase;
    int linear_counter;
    blip_synth_t synth;
};

struct nes_noise {
    struct nes_envelope env;
    int noise;
    blip_synth_t synth;
};

struct nes_dmc {
    struct nes_osc osc;
    int address;
    int period;
    int buf;
    int bits_remain;
    int bits;
    bool buf_full;
    bool silence;
    int dac;
    nes_time_t next_irq;
    bool irq_enabled;
    bool irq_flag;
    bool pal_mode;
    bool nonlinear;
    int (*prg_reader)(void *user_data, nes_addr_t addr);
    void *prg_reader_data;
    nes_apu_t *apu;
    blip_synth_t synth;
};

struct nes_apu {
    struct nes_osc *oscs[NES_APU_OSC_COUNT];
    struct nes_square square1;
    struct nes_square square2;
    struct nes_noise noise;
    struct nes_triangle triangle;
    struct nes_dmc dmc;
    double tempo;
    nes_time_t last_time;
    nes_time_t last_dmc_time;
    nes_time_t earliest_irq;
    nes_time_t next_irq;
    int frame_period;
    int frame_delay;
    int frame;
    int osc_enables;
    int frame_mode;
    bool irq_flag;
    blip_synth_t square_synth;
};

#endif
