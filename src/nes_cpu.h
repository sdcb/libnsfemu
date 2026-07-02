#ifndef NSFEMU_NES_CPU_H
#define NSFEMU_NES_CPU_H

#include "nes_apu.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    NES_CPU_PAGE_BITS = 11,
    NES_CPU_PAGE_SIZE = 0x800,
    NES_CPU_PAGE_COUNT = 0x10000 >> NES_CPU_PAGE_BITS,
    NES_CPU_IRQ_INHIBIT = 0x04,
    NES_CPU_BAD_OPCODE = 0xF2,
    NES_FUTURE_TIME = INT32_MAX / 2 + 1
};

typedef struct nes_cpu_registers {
    uint16_t pc;
    uint8_t a;
    uint8_t x;
    uint8_t y;
    uint8_t status;
    uint8_t sp;
} nes_cpu_registers_t;

typedef struct nes_cpu_state {
    const uint8_t *code_map[NES_CPU_PAGE_COUNT + 1];
    nes_time_t base;
    int time;
} nes_cpu_state_t;

typedef struct nes_cpu {
    uint8_t low_mem[0x800];
    nes_cpu_registers_t r;
    nes_cpu_state_t *state;
    nes_cpu_state_t state_storage;
    nes_time_t irq_time;
    nes_time_t end_time;
    unsigned long error_count;
    void *user_data;
    int (*read)(void *user_data, nes_addr_t addr);
    void (*write)(void *user_data, nes_addr_t addr, int data);
} nes_cpu_t;

void nes_cpu_init(nes_cpu_t *cpu);
void nes_cpu_set_callbacks(
    nes_cpu_t *cpu,
    void *user_data,
    int (*read)(void *user_data, nes_addr_t addr),
    void (*write)(void *user_data, nes_addr_t addr, int data));
void nes_cpu_reset(nes_cpu_t *cpu, const void *unmapped_page);
void nes_cpu_map_code(nes_cpu_t *cpu, nes_addr_t start, unsigned size, const void *code, bool mirror);
const uint8_t *nes_cpu_get_code(nes_cpu_t *cpu, nes_addr_t addr);
bool nes_cpu_run(nes_cpu_t *cpu, nes_time_t end_time);
nes_time_t nes_cpu_time(const nes_cpu_t *cpu);
void nes_cpu_set_time(nes_cpu_t *cpu, nes_time_t time);
void nes_cpu_set_end_time(nes_cpu_t *cpu, nes_time_t time);
void nes_cpu_clear_error_count(nes_cpu_t *cpu);

#endif
