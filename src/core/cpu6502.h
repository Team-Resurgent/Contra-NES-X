/* cpu6502.h -- the NES's CPU: a 6502 (the 2A03's, which has no decimal
 * mode). Every documented instruction, with its cycle count and the extra
 * cycle a page crossing or a taken branch costs; the undocumented NOPs, and
 * LAX / SAX / DCP / ISC / SLO / RLA / SRE / RRA, which some games use. An
 * opcode the 6502 jams on stops it (cpu->jammed) and is reported.
 *
 * Instruction-stepped: cpu_step runs one instruction, or takes the NMI / IRQ
 * waiting before it, and says how many cycles that took; the machine
 * (nes.c) runs the PPU and the APU on by as many. */
#ifndef CNX_CPU6502_H
#define CNX_CPU6502_H

#include <stdint.h>

#define CPU_C 0x01
#define CPU_Z 0x02
#define CPU_I 0x04
#define CPU_D 0x08
#define CPU_B 0x10
#define CPU_U 0x20
#define CPU_V 0x40
#define CPU_N 0x80

typedef struct Cpu6502
{
    uint8_t a, x, y, s, p;
    uint16_t pc;
    /* NMI: an edge, latched until taken. IRQ: a level, nonzero while some
     * source holds it */
    int nmi_pending;
    int irq_line;
    int jammed;
    uint32_t cycles;            /* since power on, wrapping */

    /* the bus (pointers: put back by the machine after a state load) */
    void *ctx;
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t v);
} Cpu6502;

void cpu_reset(Cpu6502 *c);
/* one instruction (or the interrupt before it): the cycles it took */
int cpu_step(Cpu6502 *c);

#endif /* CNX_CPU6502_H */
