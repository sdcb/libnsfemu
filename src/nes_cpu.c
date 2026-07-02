#include "nes_cpu.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

enum {
    ST_N = 0x80,
    ST_V = 0x40,
    ST_R = 0x20,
    ST_B = 0x10,
    ST_D = 0x08,
    ST_I = 0x04,
    ST_Z = 0x02,
    ST_C = 0x01
};

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void set_code_page(nes_cpu_t *cpu, int i, const void *p)
{
    cpu->state->code_map[i] = (const uint8_t *)p - (i * NES_CPU_PAGE_SIZE & (NES_CPU_PAGE_SIZE - 1));
}

static int update_end_time(nes_cpu_t *cpu, nes_time_t t, nes_time_t irq)
{
    if (irq < t && !(cpu->r.status & NES_CPU_IRQ_INHIBIT)) {
        /* IRQ can shorten the run window unless the emulated I flag masks it. */
        t = irq;
    }
    int delta = cpu->state->base - t;
    cpu->state->base = t;
    return delta;
}

void nes_cpu_init(nes_cpu_t *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->state = &cpu->state_storage;
}

void nes_cpu_set_callbacks(
    nes_cpu_t *cpu,
    void *user_data,
    int (*read)(void *, nes_addr_t),
    void (*write)(void *, nes_addr_t, int))
{
    cpu->user_data = user_data;
    cpu->read = read;
    cpu->write = write;
}

void nes_cpu_reset(nes_cpu_t *cpu, const void *unmapped_page)
{
    cpu->state = &cpu->state_storage;
    cpu->r.status = ST_I;
    cpu->r.sp = 0xFF;
    cpu->r.pc = 0;
    cpu->r.a = 0;
    cpu->r.x = 0;
    cpu->r.y = 0;
    cpu->state_storage.time = 0;
    cpu->state_storage.base = 0;
    cpu->irq_time = NES_FUTURE_TIME;
    cpu->end_time = NES_FUTURE_TIME;
    cpu->error_count = 0;

    set_code_page(cpu, NES_CPU_PAGE_COUNT, unmapped_page);
    nes_cpu_map_code(cpu, 0x2000, 0xE000, unmapped_page, true);
    nes_cpu_map_code(cpu, 0x0000, 0x2000, cpu->low_mem, true);
}

void nes_cpu_map_code(nes_cpu_t *cpu, nes_addr_t start, unsigned size, const void *code, bool mirror)
{
    assert(start % NES_CPU_PAGE_SIZE == 0);
    assert(size % NES_CPU_PAGE_SIZE == 0);
    assert(start + size <= 0x10000);

    unsigned page = start / NES_CPU_PAGE_SIZE;
    const uint8_t *data = (const uint8_t *)code;
    for (unsigned n = size / NES_CPU_PAGE_SIZE; n; --n) {
        set_code_page(cpu, (int)page++, data);
        if (!mirror) {
            /* ROM/SRAM advances page by page; low RAM and unmapped pages mirror. */
            data += NES_CPU_PAGE_SIZE;
        }
    }
}

const uint8_t *nes_cpu_get_code(nes_cpu_t *cpu, nes_addr_t addr)
{
    return cpu->state->code_map[addr >> NES_CPU_PAGE_BITS] + (addr & (NES_CPU_PAGE_SIZE - 1));
}

nes_time_t nes_cpu_time(const nes_cpu_t *cpu)
{
    return cpu->state->time + cpu->state->base;
}

void nes_cpu_set_time(nes_cpu_t *cpu, nes_time_t time)
{
    cpu->state->time = time - cpu->state->base;
}

void nes_cpu_set_end_time(nes_cpu_t *cpu, nes_time_t time)
{
    cpu->state->time += update_end_time(cpu, (cpu->end_time = time), cpu->irq_time);
}

void nes_cpu_clear_error_count(nes_cpu_t *cpu)
{
    cpu->error_count = 0;
}

#define PAGE_OFFSET(addr) ((addr) & (NES_CPU_PAGE_SIZE - 1))
#define TIME (s_time + s.base)
#define READ(addr) cpu->read(cpu->user_data, (addr))
#define WRITE(addr, data) cpu->write(cpu->user_data, (addr), (data))
#define READ_LOW(addr) (cpu->low_mem[(int)(addr)])
#define WRITE_LOW(addr, data) (READ_LOW(addr) = (uint8_t)(data))
#define READ_PROG(addr) (s.code_map[(addr) >> NES_CPU_PAGE_BITS][PAGE_OFFSET(addr)])
#define FLUSH_TIME() (s.time = s_time)
#define CACHE_TIME() (s_time = s.time)
#define SET_SP(v) (sp = ((v) + 1) | 0x100)
#define GET_SP() ((sp - 1) & 0xFF)
#define PUSH(v) do { sp = (sp - 1) | 0x100; WRITE_LOW(sp, v); } while (0)

bool nes_cpu_run(nes_cpu_t *cpu, nes_time_t run_end_time)
{
    nes_cpu_set_end_time(cpu, run_end_time);
    nes_cpu_state_t s = cpu->state_storage;
    cpu->state = &s;
    int16_t s_time = (int16_t)s.time;

    uint16_t pc = cpu->r.pc;
    uint8_t a = cpu->r.a;
    uint8_t x = cpu->r.x;
    uint8_t y = cpu->r.y;
    uint16_t sp;
    SET_SP(cpu->r.sp);

#define IS_NEG (nz & 0x8080)
#define CALC_STATUS(out) do { \
    (out) = status & (ST_V | ST_D | ST_I); \
    (out) |= ((nz >> 8) | nz) & ST_N; \
    (out) |= c >> 8 & ST_C; \
    if (!(nz & 0xFF)) (out) |= ST_Z; \
} while (0)
#define SET_STATUS(in) do { \
    status = (uint8_t)((in) & (ST_V | ST_D | ST_I)); \
    nz = (uint16_t)((in) << 8); \
    c = nz; \
    nz |= (uint16_t)(~(in) & ST_Z); \
} while (0)

    uint8_t status;
    uint16_t c;
    uint16_t nz;
    SET_STATUS(cpu->r.status);

    goto loop;
dec_clock_loop:
    s_time--;
loop:
    assert((unsigned)GET_SP() < 0x100);

    const uint8_t *instr = s.code_map[pc >> NES_CPU_PAGE_BITS] + PAGE_OFFSET(pc);
    uint8_t opcode = *instr++;
    pc++;

    static const uint8_t clock_table[256] = {
        0,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
        3,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
        3,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
        3,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
        3,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
        3,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
        2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
        3,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
        2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
        3,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
        3,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7
    };

    uint16_t data = clock_table[opcode];
    if ((s_time += (int16_t)data) >= 0) {
        if (s_time < (int)data) {
            /* The optimistic clock-table add may still leave enough time for
               this instruction after exact handling below. */
            goto almost_out_of_time;
        }
        s_time -= (int16_t)data;
        goto out_of_time;
    }
almost_out_of_time:
    data = *instr;

#define GET_MSB() (instr[1])
#define ADD_PAGE() do { pc++; data = (uint16_t)(data + 0x100 * GET_MSB()); } while (0)
#define GET_ADDR() get_le16(instr)
#define NO_PAGE_CROSSING(lsb) ((void)0)
#define HANDLE_PAGE_CROSSING(lsb) (s_time += (uint16_t)(lsb) >> 8)
#define INC_DEC_XY(reg, n) do { reg = (uint8_t)(nz = (uint16_t)(reg + (n))); goto loop; } while (0)
#define IND_Y(cross, out) do { \
    uint16_t temp = (uint16_t)(READ_LOW(data) + y); \
    out = (uint16_t)(temp + 0x100 * READ_LOW((uint8_t)(data + 1))); \
    cross(temp); \
} while (0)
#define IND_X(out) do { \
    uint16_t temp = (uint16_t)(data + x); \
    out = (uint16_t)(0x100 * READ_LOW((uint8_t)(temp + 1)) + READ_LOW((uint8_t)temp)); \
} while (0)
#define ARITH_ADDR_MODES(op) \
case op - 0x04: \
    IND_X(data); \
    goto ptr##op; \
case op + 0x0C: \
    IND_Y(HANDLE_PAGE_CROSSING, data); \
    goto ptr##op; \
case op + 0x10: \
    data = (uint8_t)(data + x); \
case op + 0x00: \
    data = READ_LOW(data); \
    goto imm##op; \
case op + 0x14: \
    data = (uint16_t)(data + y); \
    goto ind##op; \
case op + 0x18: \
    data = (uint16_t)(data + x); \
ind##op: \
    HANDLE_PAGE_CROSSING(data); \
case op + 0x08: \
    ADD_PAGE(); \
ptr##op: \
    FLUSH_TIME(); \
    data = (uint8_t)READ(data); \
    CACHE_TIME(); \
case op + 0x04: \
imm##op:
#define BRANCH(cond) do { \
    int16_t offset = (int8_t)data; \
    uint16_t extra_clock = (uint16_t)((++pc & 0xFF) + offset); \
    if (!(cond)) goto dec_clock_loop; \
    pc = (uint16_t)(pc + offset); \
    s_time += (extra_clock >> 8) & 1; \
    goto loop; \
} while (0)

    uint16_t read_addr = 0;

    switch (opcode) {
    case 0xB5:
        a = (uint8_t)(nz = READ_LOW((uint8_t)(data + x)));
        pc++;
        goto loop;
    case 0xA5:
        a = (uint8_t)(nz = READ_LOW(data));
        pc++;
        goto loop;
    case 0xD0:
        BRANCH((uint8_t)nz);
    case 0x20: {
        uint16_t temp = pc + 1;
        pc = GET_ADDR();
        WRITE_LOW(0x100 | (sp - 1), temp >> 8);
        sp = (sp - 2) | 0x100;
        WRITE_LOW(sp, temp);
        goto loop;
    }
    case 0x4C:
        pc = GET_ADDR();
        goto loop;
    case 0xE8:
        INC_DEC_XY(x, 1);
    case 0x10:
        BRANCH(!IS_NEG);

    ARITH_ADDR_MODES(0xC5)
        nz = (uint16_t)(a - data);
        pc++;
        c = (uint16_t)~nz;
        nz &= 0xFF;
        goto loop;

    case 0x30:
        BRANCH(IS_NEG);
    case 0xF0:
        BRANCH(!(uint8_t)nz);
    case 0x95:
        data = (uint8_t)(data + x);
    case 0x85:
        pc++;
        WRITE_LOW(data, a);
        goto loop;
    case 0xC8:
        INC_DEC_XY(y, 1);
    case 0xA8:
        y = a;
        nz = a;
        goto loop;
    case 0x98:
        a = y;
        nz = y;
        goto loop;
    case 0xAD: {
        unsigned addr = GET_ADDR();
        pc += 2;
        FLUSH_TIME();
        nz = (uint8_t)READ(addr);
        CACHE_TIME();
        a = (uint8_t)nz;
        goto loop;
    }
    case 0x60:
        pc = (uint16_t)(1 + READ_LOW(sp));
        pc = (uint16_t)(pc + 0x100 * READ_LOW(0x100 | (sp - 0xFF)));
        sp = (sp - 0xFE) | 0x100;
        goto loop;

    case 0x99: {
        uint16_t addr = (uint16_t)(y + GET_ADDR());
        pc += 2;
        if (addr <= 0x7FF) {
            /* Hot path: low RAM writes do not need the external memory callback. */
            WRITE_LOW(addr, a);
            goto loop;
        }
        FLUSH_TIME();
        WRITE(addr, a);
        CACHE_TIME();
        goto loop;
    }
    case 0x8D: {
        uint16_t addr = GET_ADDR();
        pc += 2;
        if (addr <= 0x7FF) {
            WRITE_LOW(addr, a);
            goto loop;
        }
        FLUSH_TIME();
        WRITE(addr, a);
        CACHE_TIME();
        goto loop;
    }
    case 0x9D: {
        uint16_t addr = (uint16_t)(x + GET_ADDR());
        pc += 2;
        if (addr <= 0x7FF) {
            WRITE_LOW(addr, a);
            goto loop;
        }
        FLUSH_TIME();
        WRITE(addr, a);
        CACHE_TIME();
        goto loop;
    }
    case 0x91: {
        uint16_t addr;
        IND_Y(NO_PAGE_CROSSING, addr);
        pc++;
        FLUSH_TIME();
        WRITE(addr, a);
        CACHE_TIME();
        goto loop;
    }
    case 0x81: {
        uint16_t addr;
        IND_X(addr);
        pc++;
        FLUSH_TIME();
        WRITE(addr, a);
        CACHE_TIME();
        goto loop;
    }
    case 0xA9:
        pc++;
        a = (uint8_t)data;
        nz = data;
        goto loop;

    case 0xA1: {
        uint16_t addr;
        IND_X(addr);
        pc++;
        read_addr = addr;
        goto a_nz_read_addr;
    }
    case 0xB1: {
        uint16_t addr = (uint16_t)(READ_LOW(data) + y);
        HANDLE_PAGE_CROSSING(addr);
        addr = (uint16_t)(addr + 0x100 * READ_LOW((uint8_t)(data + 1)));
        pc++;
        a = (uint8_t)(nz = READ_PROG(addr));
        if ((addr ^ 0x8000) <= 0x9FFF) {
            /* Most NSF code/data reads are in mapped ROM; avoid callback cost. */
            goto loop;
        }
        read_addr = addr;
        goto a_nz_read_addr;
    }
    case 0xB9: {
        uint16_t addr;
        HANDLE_PAGE_CROSSING(data + y);
        addr = (uint16_t)(GET_ADDR() + y);
        pc += 2;
        a = (uint8_t)(nz = READ_PROG(addr));
        if ((addr ^ 0x8000) <= 0x9FFF) {
            goto loop;
        }
        read_addr = addr;
        goto a_nz_read_addr;
    }
    case 0xBD: {
        uint16_t addr;
        HANDLE_PAGE_CROSSING(data + x);
        addr = (uint16_t)(GET_ADDR() + x);
        pc += 2;
        a = (uint8_t)(nz = READ_PROG(addr));
        if ((addr ^ 0x8000) <= 0x9FFF) {
            goto loop;
        }
        read_addr = addr;
        goto a_nz_read_addr;
    }
a_nz_read_addr:
        FLUSH_TIME();
        a = (uint8_t)(nz = READ(read_addr));
        CACHE_TIME();
        goto loop;

    case 0x50:
        BRANCH(!(status & ST_V));
    case 0x70:
        BRANCH(status & ST_V);
    case 0xB0:
        BRANCH(c & 0x100);
    case 0x90:
        BRANCH(!(c & 0x100));

    case 0x94:
        data = (uint8_t)(data + x);
    case 0x84:
        pc++;
        WRITE_LOW(data, y);
        goto loop;
    case 0x96:
        data = (uint8_t)(data + y);
    case 0x86:
        pc++;
        WRITE_LOW(data, x);
        goto loop;
    case 0xB6:
        data = (uint8_t)(data + y);
    case 0xA6:
        data = READ_LOW(data);
    case 0xA2:
        pc++;
        x = (uint8_t)data;
        nz = data;
        goto loop;
    case 0xB4:
        data = (uint8_t)(data + x);
    case 0xA4:
        data = READ_LOW(data);
    case 0xA0:
        pc++;
        y = (uint8_t)data;
        nz = data;
        goto loop;
    case 0xBC:
        data = (uint16_t)(data + x);
        HANDLE_PAGE_CROSSING(data);
    case 0xAC: {
        unsigned addr = data + 0x100 * GET_MSB();
        pc += 2;
        FLUSH_TIME();
        y = (uint8_t)(nz = READ(addr));
        CACHE_TIME();
        goto loop;
    }
    case 0xBE:
        data = (uint16_t)(data + y);
        HANDLE_PAGE_CROSSING(data);
    case 0xAE: {
        unsigned addr = data + 0x100 * GET_MSB();
        pc += 2;
        FLUSH_TIME();
        x = (uint8_t)(nz = READ(addr));
        CACHE_TIME();
        goto loop;
    }
    case 0x8C: {
        unsigned addr = GET_ADDR();
        pc += 2;
        if (addr <= 0x7FF) {
            WRITE_LOW(addr, y);
            goto loop;
        }
        FLUSH_TIME();
        WRITE(addr, y);
        CACHE_TIME();
        goto loop;
    }
    case 0x8E: {
        unsigned addr = GET_ADDR();
        pc += 2;
        if (addr <= 0x7FF) {
            WRITE_LOW(addr, x);
            goto loop;
        }
        FLUSH_TIME();
        WRITE(addr, x);
        CACHE_TIME();
        goto loop;
    }

    case 0xEC: {
        unsigned addr = GET_ADDR();
        pc++;
        FLUSH_TIME();
        data = (uint8_t)READ(addr);
        CACHE_TIME();
        goto cpx_data;
    }
    case 0xE4:
        data = READ_LOW(data);
    case 0xE0:
cpx_data:
        nz = (uint16_t)(x - data);
        pc++;
        c = (uint16_t)~nz;
        nz &= 0xFF;
        goto loop;
    case 0xCC: {
        unsigned addr = GET_ADDR();
        pc++;
        FLUSH_TIME();
        data = (uint8_t)READ(addr);
        CACHE_TIME();
        goto cpy_data;
    }
    case 0xC4:
        data = READ_LOW(data);
    case 0xC0:
cpy_data:
        nz = (uint16_t)(y - data);
        pc++;
        c = (uint16_t)~nz;
        nz &= 0xFF;
        goto loop;

    ARITH_ADDR_MODES(0x25)
        nz = (a &= (uint8_t)data);
        pc++;
        goto loop;
    ARITH_ADDR_MODES(0x45)
        nz = (a ^= (uint8_t)data);
        pc++;
        goto loop;
    ARITH_ADDR_MODES(0x05)
        nz = (a |= (uint8_t)data);
        pc++;
        goto loop;

    case 0x2C: {
        unsigned addr = GET_ADDR();
        pc += 2;
        status &= (uint8_t)~ST_V;
        FLUSH_TIME();
        nz = (uint8_t)READ(addr);
        CACHE_TIME();
        status |= nz & ST_V;
        if (a & nz) {
            goto loop;
        }
        nz <<= 8;
        goto loop;
    }
    case 0x24:
        nz = READ_LOW(data);
        pc++;
        status &= (uint8_t)~ST_V;
        status |= nz & ST_V;
        if (a & nz) {
            goto loop;
        }
        nz <<= 8;
        goto loop;

    ARITH_ADDR_MODES(0xE5)
    case 0xEB:
        data ^= 0xFF;
        goto adc_imm;
    ARITH_ADDR_MODES(0x65)
adc_imm: {
        int16_t carry = c >> 8 & 1;
        int16_t ov = (int16_t)((a ^ 0x80) + carry + (int8_t)data);
        status &= (uint8_t)~ST_V;
        status |= (uint8_t)((ov >> 2) & 0x40);
        c = nz = (uint16_t)(a + data + carry);
        pc++;
        a = (uint8_t)nz;
        goto loop;
    }

    case 0x4A:
        c = 0;
    case 0x6A:
        nz = (uint16_t)((c >> 1 & 0x80) | (a >> 1));
        c = (uint16_t)(a << 8);
        a = (uint8_t)nz;
        goto loop;
    case 0x0A:
        nz = (uint16_t)(a << 1);
        c = nz;
        a = (uint8_t)nz;
        goto loop;
    case 0x2A: {
        nz = (uint16_t)(a << 1);
        int16_t temp = c >> 8 & 1;
        c = nz;
        nz |= (uint16_t)temp;
        a = (uint8_t)nz;
        goto loop;
    }

    case 0x5E:
        data = (uint16_t)(data + x);
    case 0x4E:
        c = 0;
    case 0x6E:
        goto ror_abs;
    case 0x3E:
        data = (uint16_t)(data + x);
        goto rol_abs;
    case 0x1E:
        data = (uint16_t)(data + x);
    case 0x0E:
        c = 0;
    case 0x2E:
rol_abs:
        ADD_PAGE();
        nz = c >> 8 & 1;
        FLUSH_TIME();
        nz |= (uint16_t)(c = (uint16_t)(READ(data) << 1));
        pc++;
        WRITE(data, (uint8_t)nz);
        CACHE_TIME();
        goto loop;
ror_abs: {
        ADD_PAGE();
        FLUSH_TIME();
        int temp = READ(data);
        nz = (uint16_t)((c >> 1 & 0x80) | (temp >> 1));
        c = (uint16_t)(temp << 8);
        pc++;
        WRITE(data, (uint8_t)nz);
        CACHE_TIME();
        goto loop;
    }
    case 0x7E:
        data = (uint16_t)(data + x);
        goto ror_abs;
    case 0x76:
        data = (uint8_t)(data + x);
        goto ror_zp;
    case 0x56:
        data = (uint8_t)(data + x);
    case 0x46:
        c = 0;
    case 0x66:
ror_zp: {
        int temp = READ_LOW(data);
        nz = (uint16_t)((c >> 1 & 0x80) | (temp >> 1));
        c = (uint16_t)(temp << 8);
        goto write_nz_zp;
    }
    case 0x36:
        data = (uint8_t)(data + x);
        goto rol_zp;
    case 0x16:
        data = (uint8_t)(data + x);
    case 0x06:
        c = 0;
    case 0x26:
rol_zp:
        nz = c >> 8 & 1;
        nz |= (uint16_t)(c = (uint16_t)(READ_LOW(data) << 1));
        goto write_nz_zp;

    case 0xCA:
        INC_DEC_XY(x, -1);
    case 0x88:
        INC_DEC_XY(y, -1);
    case 0xF6:
        data = (uint8_t)(data + x);
    case 0xE6:
        nz = 1;
        goto add_nz_zp;
    case 0xD6:
        data = (uint8_t)(data + x);
    case 0xC6:
        nz = (uint16_t)-1;
add_nz_zp:
        nz = (uint16_t)(nz + READ_LOW(data));
write_nz_zp:
        pc++;
        WRITE_LOW(data, nz);
        goto loop;

    case 0xFE:
        data = (uint16_t)(x + GET_ADDR());
        goto inc_ptr;
    case 0xEE:
        data = GET_ADDR();
inc_ptr:
        nz = 1;
        goto inc_common;
    case 0xDE:
        data = (uint16_t)(x + GET_ADDR());
        goto dec_ptr;
    case 0xCE:
        data = GET_ADDR();
dec_ptr:
        nz = (uint16_t)-1;
inc_common:
        FLUSH_TIME();
        nz = (uint16_t)(nz + READ(data));
        pc += 2;
        WRITE(data, (uint8_t)nz);
        CACHE_TIME();
        goto loop;

    case 0xAA:
        x = a;
        nz = a;
        goto loop;
    case 0x8A:
        a = x;
        nz = x;
        goto loop;
    case 0x9A:
        SET_SP(x);
        goto loop;
    case 0xBA:
        x = (uint8_t)(nz = GET_SP());
        goto loop;
    case 0x48:
        PUSH(a);
        goto loop;
    case 0x68:
        a = (uint8_t)(nz = READ_LOW(sp));
        sp = (sp - 0xFF) | 0x100;
        goto loop;
    case 0x40: {
        uint8_t temp = READ_LOW(sp);
        pc = READ_LOW(0x100 | (sp - 0xFF));
        pc = (uint16_t)(pc | READ_LOW(0x100 | (sp - 0xFE)) * 0x100);
        sp = (sp - 0xFD) | 0x100;
        data = status;
        SET_STATUS(temp);
        if (!((data ^ status) & ST_I)) {
            goto loop;
        }
        cpu->r.status = status;
        int32_t delta = s.base - cpu->irq_time;
        if (delta <= 0) {
            goto loop;
        }
        if (status & ST_I) {
            goto loop;
        }
        s_time = (int16_t)(s_time + delta);
        s.base = cpu->irq_time;
        goto loop;
    }
    case 0x28: {
        uint8_t temp = READ_LOW(sp);
        sp = (sp - 0xFF) | 0x100;
        uint8_t changed = status ^ temp;
        SET_STATUS(temp);
        if (!(changed & ST_I)) {
            goto loop;
        }
        if (status & ST_I) {
            goto handle_sei;
        }
        goto handle_cli;
    }
    case 0x08: {
        uint8_t temp;
        CALC_STATUS(temp);
        PUSH(temp | (ST_B | ST_R));
        goto loop;
    }
    case 0x6C: {
        data = GET_ADDR();
        const uint8_t *page = s.code_map[data >> NES_CPU_PAGE_BITS];
        pc = page[PAGE_OFFSET(data)];
        data = (uint16_t)((data & 0xFF00) | ((data + 1) & 0xFF));
        pc = (uint16_t)(pc | (page[PAGE_OFFSET(data)] << 8));
        goto loop;
    }
    case 0x00:
        goto handle_brk;
    case 0x38:
        c = (uint16_t)~0;
        goto loop;
    case 0x18:
        c = 0;
        goto loop;
    case 0xB8:
        status &= (uint8_t)~ST_V;
        goto loop;
    case 0xD8:
        status &= (uint8_t)~ST_D;
        goto loop;
    case 0xF8:
        status |= ST_D;
        goto loop;
    case 0x58:
        if (!(status & ST_I)) {
            goto loop;
        }
        status &= (uint8_t)~ST_I;
handle_cli: {
        /* libgme intentionally does not model delayed CLI/SEI fully; preserving
           that behavior is required for byte-identical output. */
        cpu->r.status = status;
        int32_t delta = s.base - cpu->irq_time;
        if (delta <= 0) {
            if (TIME < cpu->irq_time) {
                goto loop;
            }
            goto delayed_cli;
        }
        s.base = cpu->irq_time;
        s_time = (int16_t)(s_time + delta);
        if (s_time < 0) {
            goto loop;
        }
        if (delta >= s_time + 1) {
            s.base += s_time + 1;
            s_time = -1;
            goto loop;
        }
delayed_cli:
        goto loop;
    }
    case 0x78:
        if (status & ST_I) {
            goto loop;
        }
        status |= ST_I;
handle_sei: {
        cpu->r.status = status;
        int32_t delta = s.base - cpu->end_time;
        s.base = cpu->end_time;
        s_time = (int16_t)(s_time + delta);
        if (s_time < 0) {
            goto loop;
        }
        goto loop;
    }

    case 0xB3: {
        uint16_t addr = (uint16_t)(READ_LOW(data) + y);
        HANDLE_PAGE_CROSSING(addr);
        addr = (uint16_t)(addr + 0x100 * READ_LOW((uint8_t)(data + 1)));
        pc++;
        a = x = (uint8_t)(nz = READ_PROG(addr));
        if ((addr ^ 0x8000) <= 0x9FFF) {
            goto loop;
        }
        FLUSH_TIME();
        a = x = (uint8_t)(nz = READ(addr));
        CACHE_TIME();
        goto loop;
    }
    case 0x8F: {
        uint16_t addr = GET_ADDR();
        uint8_t temp = a & x;
        pc += 2;
        if (addr <= 0x7FF) {
            WRITE_LOW(addr, temp);
            goto loop;
        }
        FLUSH_TIME();
        WRITE(addr, temp);
        CACHE_TIME();
        goto loop;
    }
    case 0xCB:
        x = (uint8_t)(nz = (uint16_t)((a & x) - data));
        pc++;
        c = (uint16_t)~nz;
        nz &= 0xFF;
        goto loop;

    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
        HANDLE_PAGE_CROSSING(data + x);
    case 0x0C:
        pc++;
    case 0x74: case 0x04: case 0x14: case 0x34: case 0x44: case 0x54: case 0x64:
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xD4: case 0xE2: case 0xF4:
        /* Common unofficial NOP/SKB/SKW opcodes appear in NSF drivers. */
        pc++;
        goto loop;
    case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
        goto loop;
    case NES_CPU_BAD_OPCODE:
        pc--;
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
    case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2:
        goto stop;

    case 0xFF:
        c |= 1;
    default: {
        /* Unknown opcodes are skipped using libgme's length table so playback can
           continue best-effort and still flag an emulation warning upstream. */
        static const unsigned char illop_lens[8] = {
            0x40, 0x40, 0x40, 0x80, 0x40, 0x40, 0x80, 0xA0
        };
        uint8_t ill = instr[-1];
        int16_t len = (illop_lens[ill >> 2 & 7] >> (ill << 1 & 6)) & 3;
        if (ill == 0x9C) {
            len = 2;
        }
        pc = (uint16_t)(pc + len);
        cpu->error_count++;
        if ((ill >> 4) == 0x0B) {
            if (ill != 0xB7) {
                HANDLE_PAGE_CROSSING(data + y);
            }
        }
        goto loop;
    }
    }

handle_brk:
    pc++;
    data = 4;
    goto interrupt;

out_of_time:
    pc--;
    FLUSH_TIME();
    data = (uint16_t)-1;
    CACHE_TIME();
    if ((int16_t)data >= 0) {
        goto interrupt;
    }
    if (s_time < 0) {
        goto loop;
    }
    goto stop;

interrupt:
    s_time += 7;
    WRITE_LOW(0x100 | (sp - 1), pc >> 8);
    WRITE_LOW(0x100 | (sp - 2), pc);
    pc = get_le16(&READ_PROG(0xFFFA) + data);
    sp = (sp - 3) | 0x100;
    {
        uint8_t temp;
        CALC_STATUS(temp);
        temp |= ST_R;
        if (data) {
            temp |= ST_B;
        }
        WRITE_LOW(sp, temp);
    }
    cpu->r.status = status |= ST_I;
    {
        int32_t delta = s.base - cpu->end_time;
        if (delta >= 0) {
            goto loop;
        }
        s_time = (int16_t)(s_time + delta);
        s.base = cpu->end_time;
        goto loop;
    }

stop:
    s.time = s_time;
    cpu->r.pc = pc;
    cpu->r.sp = (uint8_t)GET_SP();
    cpu->r.a = a;
    cpu->r.x = x;
    cpu->r.y = y;
    {
        uint8_t temp;
        CALC_STATUS(temp);
        cpu->r.status = temp;
    }
    cpu->state_storage = s;
    cpu->state = &cpu->state_storage;
    return s_time < 0;

#undef ARITH_ADDR_MODES
#undef BRANCH
#undef GET_ADDR
#undef GET_MSB
#undef ADD_PAGE
#undef IND_X
#undef IND_Y
#undef HANDLE_PAGE_CROSSING
#undef NO_PAGE_CROSSING
#undef INC_DEC_XY
#undef SET_STATUS
#undef CALC_STATUS
#undef IS_NEG
}

#undef PAGE_OFFSET
#undef TIME
#undef READ
#undef WRITE
#undef READ_LOW
#undef WRITE_LOW
#undef READ_PROG
#undef FLUSH_TIME
#undef CACHE_TIME
#undef SET_SP
#undef GET_SP
#undef PUSH
