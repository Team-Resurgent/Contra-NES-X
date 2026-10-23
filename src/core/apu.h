/* apu.h -- the NES's sound: the 2A03's APU.
 *
 * Two pulse channels, the triangle, the noise and the DMC (delta-modulated
 * samples, read from the cartridge through the CPU's bus: Contra's drums),
 * the frame counter that clocks their envelopes, lengths and sweeps, and
 * its IRQ and the DMC's. Run a CPU cycle at a time; the channels are mixed
 * the way the hardware's resistor ladders mix them, averaged down to the
 * output rate, and filtered as the console's output stage filters them
 * (high-passes at 90 and 440 Hz, a low-pass at 14 kHz). */
#ifndef CNX_APU_H
#define CNX_APU_H

#include <stdint.h>

#define APU_CPU_HZ 1789773

typedef struct ApuEnvelope
{
    uint8_t start, loop, constant, volume, divider, decay;
} ApuEnvelope;

typedef struct ApuPulse
{
    ApuEnvelope env;
    uint8_t enabled, duty, step, length;
    uint16_t period, timer;
    uint8_t sweep_on, sweep_period, sweep_negate, sweep_shift, sweep_div, sweep_reload;
    uint8_t ones_complement;    /* pulse 1 negates with one's complement */
} ApuPulse;

typedef struct ApuTriangle
{
    uint8_t enabled, control, length, linear, linear_load, linear_reload, step;
    uint16_t period, timer;
} ApuTriangle;

typedef struct ApuNoise
{
    ApuEnvelope env;
    uint8_t enabled, mode, length;
    uint16_t period, timer, lfsr;
} ApuNoise;

typedef struct ApuDmc
{
    uint8_t irq_on, loop, level, buffer, buffer_full, shift, bits, silent;
    uint16_t period, timer, start, address, length, remaining;
} ApuDmc;

typedef struct Apu
{
    ApuPulse pulse[2];
    ApuTriangle tri;
    ApuNoise noise;
    ApuDmc dmc;
    uint8_t five_step, irq_inhibit, frame_irq, dmc_irq;
    uint32_t frame_cycle;
    uint8_t odd;                /* the pulse and noise timers tick every other CPU cycle */
    int stall;                  /* CPU cycles the DMC's reads took (the machine collects them) */

    /* the output: averaged to `rate`, filtered */
    uint32_t rate, phase;
    float acc;
    int acc_n;
    float hp90_prev_in, hp90_out, hp440_prev_in, hp440_out, lp_out;
    int16_t *out;
    int out_n, out_max;

    /* the bus, for the DMC (pointers: put back by the machine after a state load) */
    void *ctx;
    uint8_t (*read)(void *ctx, uint16_t addr);
} Apu;

void apu_reset(Apu *a, uint32_t rate);
void apu_write(Apu *a, uint16_t addr, uint8_t v);
uint8_t apu_read_status(Apu *a);
/* run `cycles` CPU cycles; samples go to the buffer apu_begin gave */
void apu_run(Apu *a, int cycles);
/* where this frame's samples go, at most `max` of them */
void apu_begin(Apu *a, int16_t *out, int max);
/* nonzero while the frame counter or the DMC holds the CPU's IRQ */
int apu_irq(const Apu *a);

#endif /* CNX_APU_H */
