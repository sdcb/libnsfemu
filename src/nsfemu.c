#include "nsfemu.h"

#include "blip_buffer.h"
#include "nes_apu.h"
#include "nes_cpu.h"

#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(__LITTLE_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define NSFEMU_LITTLE_ENDIAN 1
#else
#define NSFEMU_LITTLE_ENDIAN 0
#endif

enum {
    NSF_HEADER_SIZE = 0x80,
    NSF_BANK_COUNT = 8,
    NSF_ROM_BEGIN = 0x8000,
    NSF_BANK_SELECT_ADDR = 0x5FF8,
    NSF_BANK_SIZE = 0x1000,
    NSF_ROM_PAD_EXTRA = 8,
    NSF_ROM_PAD_SIZE = NSF_BANK_SIZE + NSF_ROM_PAD_EXTRA,
    NSF_CLOCK_DIVISOR = 12,
    NSF_SRAM_ADDR = 0x6000,
    NSF_DEFAULT_SAMPLE_RATE = 44100,
    NSF_BUFFER_MSEC = 1000 / 20,
    NSF_SUPPORTED_CHIP_FLAGS = 0x00
};

typedef struct nsf_header {
    char tag[5];
    uint8_t vers;
    uint8_t track_count;
    uint8_t first_track;
    uint8_t load_addr[2];
    uint8_t init_addr[2];
    uint8_t play_addr[2];
    char game[32];
    char author[32];
    char copyright[32];
    uint8_t ntsc_speed[2];
    uint8_t banks[8];
    uint8_t pal_speed[2];
    uint8_t speed_flags;
    uint8_t chip_flags;
    uint8_t unused[4];
} nsf_header_t;

typedef struct nsf_rom {
    uint8_t *data;
    long data_size;
    long file_size;
    int32_t rom_addr;
    int32_t mask;
    int32_t size;
} nsf_rom_t;

struct nsfemu {
    int sample_rate;
    bool loaded;
    bool started;
    bool pal_only;
    bool apu_enabled;
    nsf_header_t header;
    nsf_rom_t rom;
    uint8_t initial_banks[NSF_BANK_COUNT];
    nes_addr_t init_addr;
    nes_addr_t play_addr;
    double clock_rate;
    nes_time_t next_play;
    nes_time_t play_period;
    int play_extra;
    int play_ready;
    nes_cpu_registers_t saved_state;
    nes_cpu_t cpu;
    nes_apu_t apu;
    blip_buffer_t blip;
    uint32_t channel_mask;
    uint8_t sram[0x2000];
    uint8_t unmapped_code[NES_CPU_PAGE_SIZE + 8];
    nsfemu_apu_write_callback_t apu_write_callback;
    void *apu_write_user_data;
    int apu_write_frame;
    int apu_write_sequence;
    bool apu_write_frame_started;
    bool activity_tracking_enabled;
    uint32_t current_play_frame;
    uint32_t last_apu_write_play_frame;
    uint32_t apu_write_count;
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void copy_nsf_text(char out[33], const char in[32])
{
    memcpy(out, in, 32);
    out[32] = '\0';
    for (int i = 31; i >= 0; --i) {
        if (out[i] == '\0' || out[i] == ' ') {
            out[i] = '\0';
        } else {
            break;
        }
    }
}

const char *nsfemu_error_string(nsfemu_error_t error)
{
    switch (error) {
    case NSFEMU_OK: return "ok";
    case NSFEMU_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case NSFEMU_ERROR_OUT_OF_MEMORY: return "out of memory";
    case NSFEMU_ERROR_BAD_NSF: return "bad or unsupported NSF";
    case NSFEMU_ERROR_UNSUPPORTED_EXPANSION: return "unsupported NSF expansion audio";
    case NSFEMU_ERROR_UNSUPPORTED_SAMPLE_RATE: return "unsupported sample rate";
    case NSFEMU_ERROR_TRACK_OUT_OF_RANGE: return "track out of range";
    case NSFEMU_ERROR_NOT_LOADED: return "NSF is not loaded";
    case NSFEMU_ERROR_NOT_STARTED: return "track is not started";
    default: return "unknown error";
    }
}

static void rom_clear(nsf_rom_t *rom)
{
    free(rom->data);
    memset(rom, 0, sizeof(*rom));
}

static nsfemu_error_t rom_load(nsf_rom_t *rom, const void *data, size_t size, nsf_header_t *header)
{
    rom_clear(rom);
    if (size <= NSF_HEADER_SIZE) {
        return NSFEMU_ERROR_BAD_NSF;
    }

    long file_offset = NSF_ROM_PAD_SIZE - NSF_HEADER_SIZE;
    if (size > (size_t)LONG_MAX - (size_t)(file_offset + NSF_ROM_PAD_SIZE)) {
        return NSFEMU_ERROR_OUT_OF_MEMORY;
    }
    long alloc_size = (long)size + file_offset + NSF_ROM_PAD_SIZE;
    uint8_t *buffer = (uint8_t *)malloc((size_t)alloc_size);
    if (!buffer) {
        return NSFEMU_ERROR_OUT_OF_MEMORY;
    }

    /* Match libgme's ROM layout: keep padding before and after file data so
       CPU bank pages can point directly into this allocation. */
    memset(buffer, 0, (size_t)NSF_ROM_PAD_SIZE);
    memcpy(buffer + file_offset, data, size);
    memset(buffer + alloc_size - NSF_ROM_PAD_SIZE, 0, (size_t)NSF_ROM_PAD_SIZE);

    rom->data = buffer;
    rom->data_size = alloc_size;
    rom->file_size = (long)size - NSF_HEADER_SIZE;
    memcpy(header, buffer + file_offset, NSF_HEADER_SIZE);
    return NSFEMU_OK;
}

static void rom_set_addr(nsf_rom_t *rom, long addr)
{
    rom->rom_addr = (int32_t)(addr - NSF_BANK_SIZE - NSF_ROM_PAD_EXTRA);

    /* Bank masking deliberately rounds up to the next power-of-two span, which
       reproduces libgme's bank wrap behavior for out-of-range bank selects. */
    long rounded = (addr + rom->file_size + NSF_BANK_SIZE - 1) / NSF_BANK_SIZE * NSF_BANK_SIZE;
    if (rounded <= 0) {
        rounded = 0;
    } else {
        int shift = 0;
        unsigned long max_addr = (unsigned long)(rounded - 1);
        while (max_addr >> shift) {
            shift++;
        }
        rom->mask = (int32_t)((1L << shift) - 1);
    }

    if (addr < 0) {
        addr = 0;
    }
    (void)addr;
    rom->size = (int32_t)rounded;

    long new_size = rounded - rom->rom_addr + NSF_ROM_PAD_EXTRA;
    if (new_size > 0 && new_size < rom->data_size) {
        uint8_t *p = (uint8_t *)realloc(rom->data, (size_t)new_size);
        if (p) {
            rom->data = p;
            rom->data_size = new_size;
        }
    }
}

static int32_t rom_mask_addr(const nsf_rom_t *rom, int32_t addr)
{
    return addr & rom->mask;
}

static uint8_t *rom_at_addr(nsf_rom_t *rom, int32_t addr)
{
    uint32_t offset = (uint32_t)(rom_mask_addr(rom, addr) - rom->rom_addr);
    if (offset > (uint32_t)(rom->data_size - NSF_ROM_PAD_SIZE)) {
        /* Out-of-range banks map to the zero-filled unmapped page, not NULL. */
        offset = 0;
    }
    return rom->data + offset;
}

static void nsfemu_apply_channel_mask(nsfemu_t *emu)
{
    for (int i = 0; i < NES_APU_OSC_COUNT; ++i) {
        blip_buffer_t *out = (emu->channel_mask & (1u << i)) ? &emu->blip : NULL;
        nes_apu_osc_output(&emu->apu, i, out);
    }
}

static int nsfemu_pcm_read(void *user_data, nes_addr_t addr)
{
    nsfemu_t *emu = (nsfemu_t *)user_data;
    return *nes_cpu_get_code(&emu->cpu, addr);
}

static int nsfemu_cpu_read(void *user_data, nes_addr_t addr)
{
    nsfemu_t *emu = (nsfemu_t *)user_data;

    /* NSF CPU reads follow NES priority order: mirrored low RAM first, then
       executable ROM pages, SRAM, APU status, and finally open bus. */
    int result = emu->cpu.low_mem[addr & 0x7FF];
    if (!(addr & 0xE000)) {
        return result;
    }

    result = *nes_cpu_get_code(&emu->cpu, addr);
    if (addr > 0x7FFF) {
        return result;
    }

    result = emu->sram[addr & (sizeof(emu->sram) - 1)];
    if (addr > 0x5FFF) {
        return result;
    }

    if (addr == NES_APU_STATUS_ADDR) {
        return emu->apu_enabled ? nes_apu_read_status(&emu->apu, nes_cpu_time(&emu->cpu)) : 0;
    }

    return addr >> 8;
}

static void nsfemu_apu_write_hook(nsfemu_t *emu, nes_addr_t addr, int data)
{
    if (emu->apu_write_callback) {
        emu->apu_write_callback(
            emu->apu_write_user_data,
            emu->apu_write_frame,
            emu->apu_write_sequence++,
            (int)addr,
            data & 0xFF);
        emu->apu_write_frame_started = true;
    }
}

static void nsfemu_apu_frame_hook(nsfemu_t *emu)
{
    if (emu->apu_write_frame_started) {
        emu->apu_write_frame++;
        emu->apu_write_sequence = 0;
        emu->apu_write_frame_started = false;
    }
}

static void nsfemu_cpu_write(void *user_data, nes_addr_t addr, int data)
{
    nsfemu_t *emu = (nsfemu_t *)user_data;

    /* SRAM and low RAM are real writable memory; everything else below is I/O
       or mapper-like behavior used by NSF drivers. */
    nes_addr_t sram_offset = addr ^ NSF_SRAM_ADDR;
    if (sram_offset < sizeof(emu->sram)) {
        emu->sram[sram_offset] = (uint8_t)data;
        return;
    }

    int low_offset = addr & 0x7FF;
    if (!(addr & 0xE000)) {
        emu->cpu.low_mem[low_offset] = (uint8_t)data;
        return;
    }

    if ((unsigned)(addr - NES_APU_START_ADDR) <= NES_APU_END_ADDR - NES_APU_START_ADDR) {
        /* Channel muting is applied only at APU output, so writes are always
           logged and still advance the APU state when audio is enabled. */
        bool track_writes = emu->activity_tracking_enabled || emu->apu_write_callback || !emu->apu_enabled;
        if (track_writes) {
            emu->last_apu_write_play_frame = emu->current_play_frame;
            emu->apu_write_count++;
        }
        if (emu->apu_write_callback) {
            nsfemu_apu_write_hook(emu, addr, data);
        }
        if (emu->apu_enabled) {
            nes_apu_write_register(&emu->apu, nes_cpu_time(&emu->cpu), addr, data);
        }
        return;
    }

    unsigned bank = addr - NSF_BANK_SELECT_ADDR;
    if (bank < NSF_BANK_COUNT) {
        /* NSF bank registers select 4 KiB windows mapped into $8000..$FFFF. */
        int32_t offset = rom_mask_addr(&emu->rom, data * (int32_t)NSF_BANK_SIZE);
        nes_cpu_map_code(&emu->cpu, (bank + 8) * NSF_BANK_SIZE, NSF_BANK_SIZE, rom_at_addr(&emu->rom, offset), false);
        return;
    }

    /* Expansion chip registers and mapper-specific writes are intentionally ignored. */
}

static void nsfemu_set_tempo(nsfemu_t *emu, double tempo)
{
    unsigned playback_rate = le16(emu->header.ntsc_speed);
    unsigned standard_rate = 0x411A;
    emu->clock_rate = 1789772.72727;
    emu->play_period = 262 * 341L * 4 - 2;

    /* PAL-only NSFs use a different CPU clock and default play-call period. */
    if (emu->pal_only) {
        emu->play_period = 33247 * NSF_CLOCK_DIVISOR;
        emu->clock_rate = 1662607.125;
        standard_rate = 0x4E20;
        playback_rate = le16(emu->header.pal_speed);
    }

    if (!playback_rate) {
        playback_rate = standard_rate;
    }

    /* Non-standard NSF speed fields are microseconds per play call. Convert
       them to the CPU time domain used by the scheduler. */
    if (playback_rate != standard_rate || tempo != 1.0) {
        emu->play_period = (nes_time_t)(playback_rate * emu->clock_rate / (1000000.0 / NSF_CLOCK_DIVISOR * tempo));
    }
    nes_apu_set_tempo(&emu->apu, tempo);
}

static nsfemu_error_t nsfemu_setup_audio(nsfemu_t *emu)
{
    const char *err = blip_buffer_set_sample_rate(&emu->blip, emu->sample_rate, NSF_BUFFER_MSEC);
    if (err) {
        return NSFEMU_ERROR_OUT_OF_MEMORY;
    }
    blip_buffer_clock_rate(&emu->blip, (uint32_t)(emu->clock_rate + 0.5));
    blip_buffer_bass_freq(&emu->blip, 80);

    blip_eq_t eq;
    blip_eq_init(&eq, -1.0);
    nes_apu_treble_eq(&emu->apu, &eq);
    nes_apu_volume(&emu->apu, 1.4);
    nsfemu_apply_channel_mask(emu);
    return NSFEMU_OK;
}

nsfemu_error_t nsfemu_create(nsfemu_t **out)
{
    if (!out) {
        return NSFEMU_ERROR_INVALID_ARGUMENT;
    }
    nsfemu_t *emu = (nsfemu_t *)calloc(1, sizeof(*emu));
    if (!emu) {
        return NSFEMU_ERROR_OUT_OF_MEMORY;
    }

    emu->sample_rate = NSF_DEFAULT_SAMPLE_RATE;
    emu->channel_mask = NSFEMU_CHANNEL_ALL;
    emu->apu_enabled = true;
    memset(emu->unmapped_code, NES_CPU_BAD_OPCODE, sizeof(emu->unmapped_code));
    nes_cpu_init(&emu->cpu);
    nes_cpu_set_callbacks(&emu->cpu, emu, nsfemu_cpu_read, nsfemu_cpu_write);
    nes_apu_init(&emu->apu);
    nes_apu_dmc_reader(&emu->apu, nsfemu_pcm_read, emu);
    blip_buffer_init(&emu->blip);
    *out = emu;
    return NSFEMU_OK;
}

void nsfemu_destroy(nsfemu_t *emu)
{
    if (!emu) {
        return;
    }
    rom_clear(&emu->rom);
    blip_buffer_destroy(&emu->blip);
    free(emu);
}

nsfemu_error_t nsfemu_set_sample_rate(nsfemu_t *emu, int sample_rate)
{
    if (!emu) {
        return NSFEMU_ERROR_INVALID_ARGUMENT;
    }
    if (sample_rate < 8000 || sample_rate > 192000) {
        return NSFEMU_ERROR_UNSUPPORTED_SAMPLE_RATE;
    }
    emu->sample_rate = sample_rate;
    if (emu->loaded) {
        return nsfemu_setup_audio(emu);
    }
    return NSFEMU_OK;
}

int nsfemu_sample_rate(const nsfemu_t *emu)
{
    return emu ? emu->sample_rate : 0;
}

nsfemu_error_t nsfemu_load_memory(nsfemu_t *emu, const void *data, size_t size, uint32_t load_flags)
{
    if (!emu || !data) {
        return NSFEMU_ERROR_INVALID_ARGUMENT;
    }

    emu->loaded = false;
    emu->started = false;
    nsfemu_error_t err = rom_load(&emu->rom, data, size, &emu->header);
    if (err != NSFEMU_OK) {
        return err;
    }
    if (memcmp(emu->header.tag, "NESM\x1A", 5) != 0 || emu->header.track_count == 0) {
        return NSFEMU_ERROR_BAD_NSF;
    }
    if ((emu->header.chip_flags & ~NSF_SUPPORTED_CHIP_FLAGS) &&
        (load_flags & NSFEMU_LOAD_REJECT_UNSUPPORTED_EXPANSION)) {
        /* Expansion audio is outside this library's scope; callers can choose
           strict rejection instead of the default silent ignore behavior. */
        return NSFEMU_ERROR_UNSUPPORTED_EXPANSION;
    }

    nes_addr_t load_addr = le16(emu->header.load_addr);
    emu->init_addr = le16(emu->header.init_addr);
    emu->play_addr = le16(emu->header.play_addr);
    if (!load_addr) {
        load_addr = NSF_ROM_BEGIN;
    }
    if (!emu->init_addr) {
        emu->init_addr = NSF_ROM_BEGIN;
    }
    if (!emu->play_addr) {
        emu->play_addr = NSF_ROM_BEGIN;
    }
    if (load_addr < NSF_ROM_BEGIN || emu->init_addr < NSF_ROM_BEGIN) {
        return NSFEMU_ERROR_BAD_NSF;
    }

    rom_set_addr(&emu->rom, load_addr % NSF_BANK_SIZE);
    int total_banks = emu->rom.size / NSF_BANK_SIZE;
    int first_bank = (load_addr - NSF_ROM_BEGIN) / NSF_BANK_SIZE;
    for (int i = 0; i < NSF_BANK_COUNT; ++i) {
        /* Non-banked NSFs still get an initial linear bank map. */
        int bank = i - first_bank;
        if (bank < 0 || bank >= total_banks) {
            bank = 0;
        }
        emu->initial_banks[i] = (uint8_t)bank;
    }
    for (int i = 0; i < NSF_BANK_COUNT; ++i) {
        if (emu->header.banks[i]) {
            /* Any non-zero bank table means the header supplies the startup map. */
            memcpy(emu->initial_banks, emu->header.banks, sizeof(emu->initial_banks));
            break;
        }
    }

    emu->pal_only = (emu->header.speed_flags & 3) == 1;
    emu->header.speed_flags = 0;
    nsfemu_set_tempo(emu, 1.0);

    if (emu->apu_enabled) {
        /* CPU-only length estimation sets apu_enabled=false, so it skips all
           synthesis setup while still running the same 6502 scheduler. */
        err = nsfemu_setup_audio(emu);
        if (err != NSFEMU_OK) {
            return err;
        }
    }

    emu->loaded = true;
    return NSFEMU_OK;
}

nsfemu_error_t nsfemu_get_info(const nsfemu_t *emu, nsfemu_info_t *out)
{
    if (!emu || !out) {
        return NSFEMU_ERROR_INVALID_ARGUMENT;
    }
    if (!emu->loaded) {
        return NSFEMU_ERROR_NOT_LOADED;
    }
    memset(out, 0, sizeof(*out));
    out->version = emu->header.vers;
    out->track_count = emu->header.track_count;
    out->first_track = emu->header.first_track;
    out->load_addr = le16(emu->header.load_addr);
    out->init_addr = le16(emu->header.init_addr);
    out->play_addr = le16(emu->header.play_addr);
    out->speed_flags = emu->header.speed_flags;
    out->chip_flags = emu->header.chip_flags;
    memcpy(out->banks, emu->header.banks, sizeof(out->banks));
    copy_nsf_text(out->title, emu->header.game);
    copy_nsf_text(out->artist, emu->header.author);
    copy_nsf_text(out->copyright, emu->header.copyright);
    return NSFEMU_OK;
}

nsfemu_error_t nsfemu_start_track(nsfemu_t *emu, int track)
{
    if (!emu) {
        return NSFEMU_ERROR_INVALID_ARGUMENT;
    }
    if (!emu->loaded) {
        return NSFEMU_ERROR_NOT_LOADED;
    }
    if (track < 0 || track >= emu->header.track_count) {
        return NSFEMU_ERROR_TRACK_OUT_OF_RANGE;
    }

    memset(emu->cpu.low_mem, 0, sizeof(emu->cpu.low_mem));
    memset(emu->sram, 0, sizeof(emu->sram));
    blip_buffer_clear(&emu->blip, 1);

    nes_cpu_reset(&emu->cpu, emu->unmapped_code);
    nes_cpu_map_code(&emu->cpu, NSF_SRAM_ADDR, sizeof(emu->sram), emu->sram, false);
    for (int i = 0; i < NSF_BANK_COUNT; ++i) {
        nsfemu_cpu_write(emu, NSF_BANK_SELECT_ADDR + i, emu->initial_banks[i]);
    }

    if (emu->apu_enabled) {
        nes_apu_reset(&emu->apu, emu->pal_only, (emu->header.speed_flags & 0x20) ? 0x3F : 0);
        nes_apu_write_register(&emu->apu, 0, 0x4015, 0x0F);
        nes_apu_write_register(&emu->apu, 0, 0x4017, (emu->header.speed_flags & 0x10) ? 0x80 : 0);
        nsfemu_apply_channel_mask(emu);
    }

    emu->play_ready = 4;
    emu->play_extra = 0;
    emu->next_play = emu->play_period / NSF_CLOCK_DIVISOR;
    emu->current_play_frame = 0;
    emu->last_apu_write_play_frame = 0;
    emu->apu_write_count = 0;
    emu->saved_state.pc = NSF_BANK_SELECT_ADDR;
    /* The fake return address points at a bad opcode, letting init/play return
       naturally through RTS without a separate interpreter mode. */
    emu->cpu.low_mem[0x1FF] = (uint8_t)((NSF_BANK_SELECT_ADDR - 1) >> 8);
    emu->cpu.low_mem[0x1FE] = (uint8_t)((NSF_BANK_SELECT_ADDR - 1) & 0xFF);
    emu->cpu.r.sp = 0xFD;
    emu->cpu.r.pc = emu->init_addr;
    emu->cpu.r.a = (uint8_t)track;
    emu->cpu.r.x = emu->pal_only ? 1 : 0;
    emu->started = true;
    return NSFEMU_OK;
}

void nsfemu_set_channel_mask(nsfemu_t *emu, uint32_t channel_mask)
{
    if (!emu) {
        return;
    }
    emu->channel_mask = channel_mask & NSFEMU_CHANNEL_ALL;
    nsfemu_apply_channel_mask(emu);
}

uint32_t nsfemu_channel_mask(const nsfemu_t *emu)
{
    return emu ? emu->channel_mask : 0;
}

void nsfemu_set_apu_write_callback(
    nsfemu_t *emu,
    nsfemu_apu_write_callback_t callback,
    void *user_data)
{
    if (!emu) {
        return;
    }
    emu->apu_write_callback = callback;
    emu->apu_write_user_data = user_data;
    emu->apu_write_frame = 0;
    emu->apu_write_sequence = 0;
    emu->apu_write_frame_started = false;
}

void nsfemu_set_activity_tracking(nsfemu_t *emu, int enabled)
{
    if (!emu) {
        return;
    }
    emu->activity_tracking_enabled = enabled != 0;
}

int nsfemu_activity_tracking(const nsfemu_t *emu)
{
    return emu && emu->activity_tracking_enabled ? 1 : 0;
}

nsfemu_error_t nsfemu_get_activity(const nsfemu_t *emu, uint32_t quiet_frames, nsfemu_activity_t *out)
{
    if (!emu || !out) {
        return NSFEMU_ERROR_INVALID_ARGUMENT;
    }
    if (!emu->started) {
        return emu->loaded ? NSFEMU_ERROR_NOT_STARTED : NSFEMU_ERROR_NOT_LOADED;
    }

    memset(out, 0, sizeof(*out));
    out->quiet_frames = quiet_frames;
    out->current_play_frame = emu->current_play_frame;
    out->last_write_play_frame = emu->last_apu_write_play_frame;
    out->writes_observed = emu->apu_write_count;

    if (!emu->apu_write_count) {
        out->status = NSFEMU_ACTIVITY_NO_WRITES;
    } else if (out->current_play_frame > out->last_write_play_frame + quiet_frames) {
        out->status = NSFEMU_ACTIVITY_ENDED;
    } else {
        out->status = NSFEMU_ACTIVITY_ACTIVE;
    }

    return NSFEMU_OK;
}

static void nsfemu_run_clocks(nsfemu_t *emu, blip_time_t *duration_io)
{
    nes_cpu_set_time(&emu->cpu, 0);
    while (nes_cpu_time(&emu->cpu) < *duration_io) {
        nes_time_t end = emu->next_play < *duration_io ? emu->next_play : *duration_io;
        nes_time_t cpu_end = nes_cpu_time(&emu->cpu) + 32767;
        if (end > cpu_end) {
            /* The CPU core uses a signed 16-bit relative time accumulator. */
            end = cpu_end;
        }
        if (nes_cpu_run(&emu->cpu, end)) {
            if (emu->cpu.r.pc != NSF_BANK_SELECT_ADDR) {
                /* Unexpected bad opcode: skip it and keep best-effort playback. */
                emu->cpu.r.pc++;
            } else {
                /* A clean return from init/play restores any interrupted play
                   routine state and resumes the frame scheduler. */
                emu->play_ready = 1;
                if (emu->saved_state.pc != NSF_BANK_SELECT_ADDR) {
                    emu->cpu.r = emu->saved_state;
                    emu->saved_state.pc = NSF_BANK_SELECT_ADDR;
                } else {
                    nes_cpu_set_time(&emu->cpu, end);
                }
            }
        }

        if (nes_cpu_time(&emu->cpu) >= emu->next_play) {
            nes_time_t period = (emu->play_period + emu->play_extra) / NSF_CLOCK_DIVISOR;
            emu->play_extra = emu->play_period - period * NSF_CLOCK_DIVISOR;
            emu->next_play += period;
            if (emu->play_ready && !--emu->play_ready) {
                /* If init/play is still running at a play boundary, save it and
                   inject the next play call exactly as libgme does. */
                if (emu->cpu.r.pc != NSF_BANK_SELECT_ADDR) {
                    emu->saved_state = emu->cpu.r;
                }

                emu->cpu.r.pc = emu->play_addr;
                emu->cpu.low_mem[0x100 + emu->cpu.r.sp--] = (uint8_t)((NSF_BANK_SELECT_ADDR - 1) >> 8);
                emu->cpu.low_mem[0x100 + emu->cpu.r.sp--] = (uint8_t)((NSF_BANK_SELECT_ADDR - 1) & 0xFF);
                nsfemu_apu_frame_hook(emu);
                emu->current_play_frame++;
            }
        }
    }

    if (emu->cpu.error_count) {
        nes_cpu_clear_error_count(&emu->cpu);
    }

    *duration_io = nes_cpu_time(&emu->cpu);
    emu->next_play -= *duration_io;
    if (emu->next_play < 0) {
        emu->next_play = 0;
    }

    if (emu->apu_enabled) {
        nes_apu_end_frame(&emu->apu, *duration_io);
    }
}

static void nsfemu_generate_more(nsfemu_t *emu)
{
    blip_time_t clocks = (blip_time_t)((int64_t)emu->blip.length * (uint32_t)(emu->clock_rate + 0.5) / 1000);
    nsfemu_run_clocks(emu, &clocks);
    blip_buffer_end_frame(&emu->blip, clocks);
}

nsfemu_error_t nsfemu_render_s16le(nsfemu_t *emu, void *out, size_t out_size, size_t frame_count, int channels)
{
    if (!emu || !out || (channels != 1 && channels != 2)) {
        return NSFEMU_ERROR_INVALID_ARGUMENT;
    }
    if (!emu->started) {
        return emu->loaded ? NSFEMU_ERROR_NOT_STARTED : NSFEMU_ERROR_NOT_LOADED;
    }
    if (out_size < frame_count * (size_t)channels * sizeof(int16_t)) {
        return NSFEMU_ERROR_INVALID_ARGUMENT;
    }

#if NSFEMU_LITTLE_ENDIAN
    int16_t *samples = (int16_t *)out;
#else
    uint8_t *bytes = (uint8_t *)out;
#endif
    size_t done = 0;
#if !NSFEMU_LITTLE_ENDIAN
    int16_t temp[1024];
#endif
    while (done < frame_count) {
        if (blip_buffer_samples_avail(&emu->blip) <= 0) {
            /* Generate just enough emulated time to refill the Blip buffer. */
            nsfemu_generate_more(emu);
        }
        long avail = blip_buffer_samples_avail(&emu->blip);
        size_t want = frame_count - done;
        if (want > (size_t)avail) {
            want = (size_t)avail;
        }
        if (want > 4096) {
            want = 4096;
        }
        long got = 0;
#if NSFEMU_LITTLE_ENDIAN
        /* The API promises little-endian PCM; on Windows and common little-endian
           targets, direct int16_t stores are already byte-correct. */
        got = blip_buffer_read_samples_interleaved(&emu->blip, samples, (long)want, channels);
        samples += got * channels;
#else
        if (want > sizeof(temp) / sizeof(temp[0])) {
            want = sizeof(temp) / sizeof(temp[0]);
        }
        got = blip_buffer_read_samples(&emu->blip, temp, (long)want);
        for (long i = 0; i < got; ++i) {
            uint16_t sample = (uint16_t)temp[i];
            *bytes++ = (uint8_t)(sample & 0xFF);
            *bytes++ = (uint8_t)(sample >> 8);
            if (channels == 2) {
                *bytes++ = (uint8_t)(sample & 0xFF);
                *bytes++ = (uint8_t)(sample >> 8);
            }
        }
#endif
        done += (size_t)got;
    }
    return NSFEMU_OK;
}

nsfemu_error_t nsfemu_probe_length(
    const void *data,
    size_t size,
    int track,
    uint32_t probe_milliseconds,
    nsfemu_length_probe_t *out)
{
    if (!data || !out || probe_milliseconds == 0) {
        return NSFEMU_ERROR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->probe_milliseconds = probe_milliseconds;

    nsfemu_t *emu = NULL;
    nsfemu_error_t err = nsfemu_create(&emu);
    if (err != NSFEMU_OK) {
        return err;
    }
    emu->apu_enabled = false;
    err = nsfemu_load_memory(emu, data, size, NSFEMU_LOAD_DEFAULT);
    if (err == NSFEMU_OK) {
        err = nsfemu_start_track(emu, track);
        if (err == NSFEMU_OK) {
            /* CPU-only heuristic: if writes stop for a few seconds before the
               caller's probe limit, classify by the last write. */
            const uint32_t chunk_msec = 50;
            const uint32_t quiet_frames = 180;
            uint32_t last_observed_frame = 0;
            for (uint32_t elapsed = 0; elapsed < probe_milliseconds;) {
                uint32_t step_msec = probe_milliseconds - elapsed;
                if (step_msec > chunk_msec) {
                    step_msec = chunk_msec;
                }
                blip_time_t clocks = (blip_time_t)((int64_t)step_msec * (uint32_t)(emu->clock_rate + 0.5) / 1000);
                nsfemu_run_clocks(emu, &clocks);
                elapsed += step_msec;
                last_observed_frame = emu->current_play_frame;
                if (emu->apu_write_count && last_observed_frame > emu->last_apu_write_play_frame + quiet_frames) {
                    out->status = NSFEMU_LENGTH_PROBE_ENDED;
                    out->last_write_milliseconds = emu->last_apu_write_play_frame * 1000u / 60u;
                    out->frames_observed = last_observed_frame;
                    out->writes_observed = emu->apu_write_count;
                    nsfemu_destroy(emu);
                    return NSFEMU_OK;
                }
            }
            out->status = emu->apu_write_count
                ? NSFEMU_LENGTH_PROBE_ACTIVE_AT_LIMIT
                : NSFEMU_LENGTH_PROBE_NO_WRITES;
            out->last_write_milliseconds = emu->last_apu_write_play_frame * 1000u / 60u;
            out->frames_observed = last_observed_frame;
            out->writes_observed = emu->apu_write_count;
        }
    }
    nsfemu_destroy(emu);
    return err;
}
