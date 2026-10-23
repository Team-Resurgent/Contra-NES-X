/* cpu6502.c -- the 2A03's 6502 (cpu6502.h). */
#include "cpu6502.h"

#define VEC_NMI 0xFFFA
#define VEC_RESET 0xFFFC
#define VEC_IRQ 0xFFFE

static inline uint8_t rd(Cpu6502 *c, uint16_t a)
{
    return c->read(c->ctx, a);
}

static inline void wr(Cpu6502 *c, uint16_t a, uint8_t v)
{
    c->write(c->ctx, a, v);
}

static inline uint16_t rd16(Cpu6502 *c, uint16_t a)
{
    return (uint16_t)(rd(c, a) | (rd(c, (uint16_t)(a + 1)) << 8));
}

static inline uint16_t fetch16(Cpu6502 *c)
{
    uint16_t v = rd16(c, c->pc);

    c->pc = (uint16_t)(c->pc + 2);
    return v;
}

static inline void push(Cpu6502 *c, uint8_t v)
{
    wr(c, (uint16_t)(0x100 | c->s), v);
    c->s--;
}

static inline uint8_t pull(Cpu6502 *c)
{
    c->s++;
    return rd(c, (uint16_t)(0x100 | c->s));
}

static inline void nz(Cpu6502 *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(CPU_N | CPU_Z)) | (v & CPU_N) | (v ? 0 : CPU_Z));
}

static inline void setc(Cpu6502 *c, int on)
{
    c->p = (uint8_t)(on ? (c->p | CPU_C) : (c->p & ~CPU_C));
}

static inline void setv(Cpu6502 *c, int on)
{
    c->p = (uint8_t)(on ? (c->p | CPU_V) : (c->p & ~CPU_V));
}

static void adc(Cpu6502 *c, uint8_t v)
{
    unsigned s = (unsigned)c->a + v + (c->p & CPU_C);

    setv(c, (~(c->a ^ v) & (c->a ^ s) & 0x80) != 0);
    setc(c, s > 0xFF);
    c->a = (uint8_t)s;
    nz(c, c->a);
}

static void cmp(Cpu6502 *c, uint8_t r, uint8_t v)
{
    setc(c, r >= v);
    nz(c, (uint8_t)(r - v));
}

static uint8_t asl(Cpu6502 *c, uint8_t v)
{
    setc(c, v & 0x80);
    v = (uint8_t)(v << 1);
    nz(c, v);
    return v;
}

static uint8_t lsr(Cpu6502 *c, uint8_t v)
{
    setc(c, v & 1);
    v >>= 1;
    nz(c, v);
    return v;
}

static uint8_t rol(Cpu6502 *c, uint8_t v)
{
    int in = c->p & CPU_C;

    setc(c, v & 0x80);
    v = (uint8_t)((v << 1) | in);
    nz(c, v);
    return v;
}

static uint8_t ror(Cpu6502 *c, uint8_t v)
{
    int in = c->p & CPU_C;

    setc(c, v & 1);
    v = (uint8_t)((v >> 1) | (in ? 0x80 : 0));
    nz(c, v);
    return v;
}

static int branch(Cpu6502 *c, int take)
{
    int8_t d = (int8_t)rd(c, c->pc++);
    uint16_t to;

    if (!take)
        return 2;
    to = (uint16_t)(c->pc + d);
    take = ((to ^ c->pc) & 0xFF00) ? 4 : 3;
    c->pc = to;
    return take;
}

static void interrupt(Cpu6502 *c, uint16_t vec, int brk)
{
    push(c, (uint8_t)(c->pc >> 8));
    push(c, (uint8_t)c->pc);
    push(c, (uint8_t)((c->p | CPU_U | (brk ? CPU_B : 0)) & ~(brk ? 0 : CPU_B)));
    c->p |= CPU_I;
    c->pc = rd16(c, vec);
}

void cpu_reset(Cpu6502 *c)
{
    c->a = c->x = c->y = 0;
    c->s = 0xFD;
    c->p = CPU_I | CPU_U;
    c->nmi_pending = 0;
    c->jammed = 0;
    c->pc = rd16(c, VEC_RESET);
}

/* the addressing modes: each leaves the operand's address in `ea`, and in
 * `pen` whether indexing crossed a page (a read then takes a cycle more) */
#define M_IMM ea = c->pc++
#define M_ZP ea = rd(c, c->pc++)
#define M_ZPX ea = (uint8_t)(rd(c, c->pc++) + c->x)
#define M_ZPY ea = (uint8_t)(rd(c, c->pc++) + c->y)
#define M_ABS ea = fetch16(c)
#define M_ABX { uint16_t b_ = fetch16(c); ea = (uint16_t)(b_ + c->x); pen = ((b_ ^ ea) & 0xFF00) != 0; }
#define M_ABY { uint16_t b_ = fetch16(c); ea = (uint16_t)(b_ + c->y); pen = ((b_ ^ ea) & 0xFF00) != 0; }
#define M_IZX { uint8_t z_ = (uint8_t)(rd(c, c->pc++) + c->x); \
                ea = (uint16_t)(rd(c, z_) | (rd(c, (uint8_t)(z_ + 1)) << 8)); }
#define M_IZY { uint8_t z_ = rd(c, c->pc++); \
                uint16_t b_ = (uint16_t)(rd(c, z_) | (rd(c, (uint8_t)(z_ + 1)) << 8)); \
                ea = (uint16_t)(b_ + c->y); pen = ((b_ ^ ea) & 0xFF00) != 0; }

/* reads: the page-crossing cycle counts */
#define RD(op, mode, cy, body) case op: { mode; v = rd(c, ea); body; cyc = cy + pen; } break;
/* writes and read-modify-writes: fixed */
#define WR(op, mode, cy, val) case op: { mode; wr(c, ea, val); cyc = cy; } break;
#define RMW(op, mode, cy, body) case op: { mode; v = rd(c, ea); body; wr(c, ea, v); cyc = cy; } break;

/* the eight read modes of the ALU group (imm zp zpx abs abx aby izx izy) */
#define ALU(base, body) \
    RD(base + 0x09, M_IMM, 2, body) RD(base + 0x05, M_ZP, 3, body) RD(base + 0x15, M_ZPX, 4, body) \
    RD(base + 0x0D, M_ABS, 4, body) RD(base + 0x1D, M_ABX, 4, body) RD(base + 0x19, M_ABY, 4, body) \
    RD(base + 0x01, M_IZX, 6, body) RD(base + 0x11, M_IZY, 5, body)
/* the shifts and INC / DEC: zp zpx abs abx */
#define SHIFT(base, body) \
    RMW(base + 0x06, M_ZP, 5, body) RMW(base + 0x16, M_ZPX, 6, body) \
    RMW(base + 0x0E, M_ABS, 6, body) RMW(base + 0x1E, { M_ABX; }, 7, body)
/* the undocumented read-modify-write combinations: zp zpx abs abx aby izx izy */
#define COMBO(base, body) \
    RMW(base + 0x07, M_ZP, 5, body) RMW(base + 0x17, M_ZPX, 6, body) \
    RMW(base + 0x0F, M_ABS, 6, body) RMW(base + 0x1F, M_ABX, 7, body) \
    RMW(base + 0x1B, M_ABY, 7, body) RMW(base + 0x03, M_IZX, 8, body) \
    RMW(base + 0x13, M_IZY, 8, body)

int cpu_step(Cpu6502 *c)
{
    uint16_t ea = 0;
    uint8_t op, v = 0;
    int cyc = 2, pen = 0;

    if (c->jammed)
        return 2;
    if (c->nmi_pending)
    {
        c->nmi_pending = 0;
        interrupt(c, VEC_NMI, 0);
        c->cycles += 7;
        return 7;
    }
    if (c->irq_line && !(c->p & CPU_I))
    {
        interrupt(c, VEC_IRQ, 0);
        c->cycles += 7;
        return 7;
    }

    op = rd(c, c->pc++);
    switch (op)
    {
        ALU(0x00, { c->a |= v; nz(c, c->a); })                  /* ORA */
        ALU(0x20, { c->a &= v; nz(c, c->a); })                  /* AND */
        ALU(0x40, { c->a ^= v; nz(c, c->a); })                  /* EOR */
        ALU(0x60, adc(c, v))                                    /* ADC */
        ALU(0xC0, cmp(c, c->a, v))                              /* CMP */
        ALU(0xE0, adc(c, (uint8_t)~v))                          /* SBC */
        ALU(0xA0, { c->a = v; nz(c, c->a); })                   /* LDA */
        RD(0xEB, M_IMM, 2, adc(c, (uint8_t)~v))                 /* SBC, undocumented */

        WR(0x85, M_ZP, 3, c->a) WR(0x95, M_ZPX, 4, c->a) WR(0x8D, M_ABS, 4, c->a)
        WR(0x9D, M_ABX, 5, c->a) WR(0x99, M_ABY, 5, c->a) WR(0x81, M_IZX, 6, c->a)
        WR(0x91, M_IZY, 6, c->a)                                /* STA */
        WR(0x86, M_ZP, 3, c->x) WR(0x96, M_ZPY, 4, c->x) WR(0x8E, M_ABS, 4, c->x)   /* STX */
        WR(0x84, M_ZP, 3, c->y) WR(0x94, M_ZPX, 4, c->y) WR(0x8C, M_ABS, 4, c->y)   /* STY */

        RD(0xA2, M_IMM, 2, { c->x = v; nz(c, v); }) RD(0xA6, M_ZP, 3, { c->x = v; nz(c, v); })
        RD(0xB6, M_ZPY, 4, { c->x = v; nz(c, v); }) RD(0xAE, M_ABS, 4, { c->x = v; nz(c, v); })
        RD(0xBE, M_ABY, 4, { c->x = v; nz(c, v); })            /* LDX */
        RD(0xA0, M_IMM, 2, { c->y = v; nz(c, v); }) RD(0xA4, M_ZP, 3, { c->y = v; nz(c, v); })
        RD(0xB4, M_ZPX, 4, { c->y = v; nz(c, v); }) RD(0xAC, M_ABS, 4, { c->y = v; nz(c, v); })
        RD(0xBC, M_ABX, 4, { c->y = v; nz(c, v); })            /* LDY */
        RD(0xE0, M_IMM, 2, cmp(c, c->x, v)) RD(0xE4, M_ZP, 3, cmp(c, c->x, v))
        RD(0xEC, M_ABS, 4, cmp(c, c->x, v))                     /* CPX */
        RD(0xC0, M_IMM, 2, cmp(c, c->y, v)) RD(0xC4, M_ZP, 3, cmp(c, c->y, v))
        RD(0xCC, M_ABS, 4, cmp(c, c->y, v))                     /* CPY */
        RD(0x24, M_ZP, 3, { c->p = (uint8_t)((c->p & ~(CPU_N | CPU_V | CPU_Z)) | (v & (CPU_N | CPU_V)) |
                                             ((c->a & v) ? 0 : CPU_Z)); })
        RD(0x2C, M_ABS, 4, { c->p = (uint8_t)((c->p & ~(CPU_N | CPU_V | CPU_Z)) | (v & (CPU_N | CPU_V)) |
                                              ((c->a & v) ? 0 : CPU_Z)); })     /* BIT */

        SHIFT(0x00, v = asl(c, v))                              /* ASL */
        SHIFT(0x20, v = rol(c, v))                              /* ROL */
        SHIFT(0x40, v = lsr(c, v))                              /* LSR */
        SHIFT(0x60, v = ror(c, v))                              /* ROR */
        SHIFT(0xC0, { v--; nz(c, v); })                         /* DEC */
        SHIFT(0xE0, { v++; nz(c, v); })                         /* INC */
    case 0x0A: c->a = asl(c, c->a); break;
    case 0x2A: c->a = rol(c, c->a); break;
    case 0x4A: c->a = lsr(c, c->a); break;
    case 0x6A: c->a = ror(c, c->a); break;

    case 0x10: cyc = branch(c, !(c->p & CPU_N)); break;     /* BPL */
    case 0x30: cyc = branch(c, (c->p & CPU_N) != 0); break; /* BMI */
    case 0x50: cyc = branch(c, !(c->p & CPU_V)); break;     /* BVC */
    case 0x70: cyc = branch(c, (c->p & CPU_V) != 0); break; /* BVS */
    case 0x90: cyc = branch(c, !(c->p & CPU_C)); break;     /* BCC */
    case 0xB0: cyc = branch(c, (c->p & CPU_C) != 0); break; /* BCS */
    case 0xD0: cyc = branch(c, !(c->p & CPU_Z)); break;     /* BNE */
    case 0xF0: cyc = branch(c, (c->p & CPU_Z) != 0); break; /* BEQ */

    case 0x4C: c->pc = fetch16(c); cyc = 3; break;          /* JMP abs */
    case 0x6C:                                              /* JMP (ind): the page does not carry */
        ea = fetch16(c);
        c->pc = (uint16_t)(rd(c, ea) | (rd(c, (uint16_t)((ea & 0xFF00) | ((ea + 1) & 0xFF))) << 8));
        cyc = 5;
        break;
    case 0x20:                                              /* JSR */
        ea = fetch16(c);
        c->pc--;
        push(c, (uint8_t)(c->pc >> 8));
        push(c, (uint8_t)c->pc);
        c->pc = ea;
        cyc = 6;
        break;
    case 0x60:                                              /* RTS */
        c->pc = pull(c);
        c->pc = (uint16_t)((c->pc | (pull(c) << 8)) + 1);
        cyc = 6;
        break;
    case 0x40:                                              /* RTI */
        c->p = (uint8_t)((pull(c) & ~CPU_B) | CPU_U);
        c->pc = pull(c);
        c->pc |= (uint16_t)(pull(c) << 8);
        cyc = 6;
        break;
    case 0x00:                                              /* BRK */
        c->pc++;
        interrupt(c, VEC_IRQ, 1);
        cyc = 7;
        break;

    case 0x48: push(c, c->a); cyc = 3; break;                                   /* PHA */
    case 0x08: push(c, (uint8_t)(c->p | CPU_B | CPU_U)); cyc = 3; break;        /* PHP */
    case 0x68: c->a = pull(c); nz(c, c->a); cyc = 4; break;                     /* PLA */
    case 0x28: c->p = (uint8_t)((pull(c) & ~CPU_B) | CPU_U); cyc = 4; break;    /* PLP */

    case 0x18: c->p &= (uint8_t)~CPU_C; break;
    case 0x38: c->p |= CPU_C; break;
    case 0x58: c->p &= (uint8_t)~CPU_I; break;
    case 0x78: c->p |= CPU_I; break;
    case 0xB8: c->p &= (uint8_t)~CPU_V; break;
    case 0xD8: c->p &= (uint8_t)~CPU_D; break;
    case 0xF8: c->p |= CPU_D; break;                        /* kept, and ignored: no decimal mode */

    case 0xAA: c->x = c->a; nz(c, c->x); break;
    case 0x8A: c->a = c->x; nz(c, c->a); break;
    case 0xA8: c->y = c->a; nz(c, c->y); break;
    case 0x98: c->a = c->y; nz(c, c->a); break;
    case 0xBA: c->x = c->s; nz(c, c->x); break;
    case 0x9A: c->s = c->x; break;
    case 0xE8: c->x++; nz(c, c->x); break;
    case 0xC8: c->y++; nz(c, c->y); break;
    case 0xCA: c->x--; nz(c, c->x); break;
    case 0x88: c->y--; nz(c, c->y); break;

    /* the NOPs, documented and not: their operand is read and dropped */
    case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
        break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
        c->pc++;
        break;
        RD(0x04, M_ZP, 3, (void)v) RD(0x44, M_ZP, 3, (void)v) RD(0x64, M_ZP, 3, (void)v)
        RD(0x14, M_ZPX, 4, (void)v) RD(0x34, M_ZPX, 4, (void)v) RD(0x54, M_ZPX, 4, (void)v)
        RD(0x74, M_ZPX, 4, (void)v) RD(0xD4, M_ZPX, 4, (void)v) RD(0xF4, M_ZPX, 4, (void)v)
        RD(0x0C, M_ABS, 4, (void)v)
        RD(0x1C, M_ABX, 4, (void)v) RD(0x3C, M_ABX, 4, (void)v) RD(0x5C, M_ABX, 4, (void)v)
        RD(0x7C, M_ABX, 4, (void)v) RD(0xDC, M_ABX, 4, (void)v) RD(0xFC, M_ABX, 4, (void)v)

    /* the stable undocumented ones */
        RD(0xA7, M_ZP, 3, { c->a = c->x = v; nz(c, v); }) RD(0xB7, M_ZPY, 4, { c->a = c->x = v; nz(c, v); })
        RD(0xAF, M_ABS, 4, { c->a = c->x = v; nz(c, v); }) RD(0xBF, M_ABY, 4, { c->a = c->x = v; nz(c, v); })
        RD(0xA3, M_IZX, 6, { c->a = c->x = v; nz(c, v); }) RD(0xB3, M_IZY, 5, { c->a = c->x = v; nz(c, v); })
                                                                /* LAX */
        WR(0x87, M_ZP, 3, c->a & c->x) WR(0x97, M_ZPY, 4, c->a & c->x)
        WR(0x8F, M_ABS, 4, c->a & c->x) WR(0x83, M_IZX, 6, c->a & c->x)   /* SAX */
        COMBO(0xC0, { v--; cmp(c, c->a, v); })                  /* DCP */
        COMBO(0xE0, { v++; adc(c, (uint8_t)~v); })              /* ISC */
        COMBO(0x00, { v = asl(c, v); c->a |= v; nz(c, c->a); }) /* SLO */
        COMBO(0x20, { v = rol(c, v); c->a &= v; nz(c, c->a); }) /* RLA */
        COMBO(0x40, { v = lsr(c, v); c->a ^= v; nz(c, c->a); }) /* SRE */
        COMBO(0x60, { v = ror(c, v); adc(c, v); })              /* RRA */
        RD(0x0B, M_IMM, 2, { c->a &= v; nz(c, c->a); setc(c, c->a & 0x80); })
        RD(0x2B, M_IMM, 2, { c->a &= v; nz(c, c->a); setc(c, c->a & 0x80); })     /* ANC */
        RD(0x4B, M_IMM, 2, { c->a = lsr(c, (uint8_t)(c->a & v)); })              /* ALR */
        RD(0x6B, M_IMM, 2, {                                                       /* ARR */
            c->a = (uint8_t)(((c->a & v) >> 1) | ((c->p & CPU_C) ? 0x80 : 0));
            nz(c, c->a);
            setc(c, c->a & 0x40);
            setv(c, ((c->a >> 6) ^ (c->a >> 5)) & 1);
        })
        RD(0xCB, M_IMM, 2, {                                                       /* AXS */
            unsigned r = (unsigned)(c->a & c->x) - v;
            setc(c, r < 0x100);
            c->x = (uint8_t)r;
            nz(c, c->x);
        })

    default:
        /* the KIL opcodes (x2), and the unstable ones no game relies on */
        c->jammed = 1;
        c->pc--;
        break;
    }
    c->cycles += (uint32_t)cyc;
    return cyc;
}
