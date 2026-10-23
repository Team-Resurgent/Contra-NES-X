/* apu.c -- the 2A03's APU (apu.h). NTSC rates and periods. */
#include "apu.h"

#include <string.h>

static const uint8_t kLength[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

static const uint8_t kDuty[4][8] = {
    { 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 1, 0, 0, 1, 1, 1, 1, 1 },
};

static const uint8_t kTriangle[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

/* in CPU cycles */
static const uint16_t kNoisePeriod[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};
static const uint16_t kDmcPeriod[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};

/* the frame counter's steps, in CPU cycles from its reset */
#define FC_Q1 7457
#define FC_H1 14913
#define FC_Q3 22371
#define FC_END4 29829
#define FC_END5 37281

/* the mixer's two ladders, as tables (filled on the first reset) */
static float sPulseMix[31], sTndMix[203];

/* ------------------------------------------------------------ the units */

static void envelope_clock(ApuEnvelope *e)
{
    if (e->start)
    {
        e->start = 0;
        e->decay = 15;
        e->divider = e->volume;
    }
    else if (e->divider == 0)
    {
        e->divider = e->volume;
        if (e->decay)
            e->decay--;
        else if (e->loop)
            e->decay = 15;
    }
    else
        e->divider--;
}

static inline uint8_t envelope_out(const ApuEnvelope *e)
{
    return e->constant ? e->volume : e->decay;
}

static uint16_t sweep_target(const ApuPulse *p)
{
    uint16_t change = (uint16_t)(p->period >> p->sweep_shift);

    if (!p->sweep_negate)
        return (uint16_t)(p->period + change);
    if (change > p->period)
        return 0;
    return (uint16_t)(p->period - change - (p->ones_complement ? 1 : 0));
}

static inline int pulse_muted(const ApuPulse *p)
{
    return p->period < 8 || (!p->sweep_negate && sweep_target(p) > 0x7FF);
}

static void sweep_clock(ApuPulse *p)
{
    if (p->sweep_div == 0 && p->sweep_on && p->sweep_shift && !pulse_muted(p))
        p->period = sweep_target(p);
    if (p->sweep_div == 0 || p->sweep_reload)
    {
        p->sweep_div = p->sweep_period;
        p->sweep_reload = 0;
    }
    else
        p->sweep_div--;
}

static void quarter_frame(Apu *a)
{
    envelope_clock(&a->pulse[0].env);
    envelope_clock(&a->pulse[1].env);
    envelope_clock(&a->noise.env);
    if (a->tri.linear_reload)
        a->tri.linear = a->tri.linear_load;
    else if (a->tri.linear)
        a->tri.linear--;
    if (!a->tri.control)
        a->tri.linear_reload = 0;
}

static void half_frame(Apu *a)
{
    int i;

    for (i = 0; i < 2; i++)
    {
        if (a->pulse[i].length && !a->pulse[i].env.loop)
            a->pulse[i].length--;
        sweep_clock(&a->pulse[i]);
    }
    if (a->tri.length && !a->tri.control)
        a->tri.length--;
    if (a->noise.length && !a->noise.env.loop)
        a->noise.length--;
}

static void frame_counter_clock(Apu *a)
{
    switch (++a->frame_cycle)
    {
    case FC_Q1:
    case FC_Q3:
        quarter_frame(a);
        break;
    case FC_H1:
        quarter_frame(a);
        half_frame(a);
        break;
    case FC_END4:
        if (!a->five_step)
        {
            quarter_frame(a);
            half_frame(a);
            if (!a->irq_inhibit)
                a->frame_irq = 1;
            a->frame_cycle = 0;
        }
        break;
    case FC_END5:
        quarter_frame(a);
        half_frame(a);
        a->frame_cycle = 0;
        break;
    default:
        break;
    }
}

/* the DMC: its sample buffer refilled from memory when it is empty */
static void dmc_fetch(Apu *a)
{
    ApuDmc *d = &a->dmc;

    if (d->buffer_full || d->remaining == 0)
        return;
    d->buffer = a->read(a->ctx, d->address);
    d->buffer_full = 1;
    a->stall += 4;
    d->address = (uint16_t)(d->address == 0xFFFF ? 0x8000 : d->address + 1);
    if (--d->remaining == 0)
    {
        if (d->loop)
        {
            d->address = d->start;
            d->remaining = d->length;
        }
        else if (d->irq_on)
            a->dmc_irq = 1;
    }
}

static void dmc_clock(Apu *a)
{
    ApuDmc *d = &a->dmc;

    if (d->timer > 1)
    {
        d->timer--;
        return;
    }
    d->timer = d->period;
    if (!d->silent)
    {
        if (d->shift & 1)
        {
            if (d->level <= 125)
                d->level += 2;
        }
        else if (d->level >= 2)
            d->level -= 2;
        d->shift >>= 1;
    }
    if (--d->bits == 0)
    {
        d->bits = 8;
        if (d->buffer_full)
        {
            d->silent = 0;
            d->shift = d->buffer;
            d->buffer_full = 0;
            dmc_fetch(a);
        }
        else
            d->silent = 1;
    }
}

/* ------------------------------------------------------------- the bus */

void apu_write(Apu *a, uint16_t addr, uint8_t v)
{
    ApuPulse *p;

    switch (addr)
    {
    case 0x4000:
    case 0x4004:
        p = &a->pulse[(addr >> 2) & 1];
        p->duty = (uint8_t)(v >> 6);
        p->env.loop = (uint8_t)((v >> 5) & 1);
        p->env.constant = (uint8_t)((v >> 4) & 1);
        p->env.volume = (uint8_t)(v & 15);
        break;
    case 0x4001:
    case 0x4005:
        p = &a->pulse[(addr >> 2) & 1];
        p->sweep_on = (uint8_t)(v >> 7);
        p->sweep_period = (uint8_t)((v >> 4) & 7);
        p->sweep_negate = (uint8_t)((v >> 3) & 1);
        p->sweep_shift = (uint8_t)(v & 7);
        p->sweep_reload = 1;
        break;
    case 0x4002:
    case 0x4006:
        p = &a->pulse[(addr >> 2) & 1];
        p->period = (uint16_t)((p->period & 0x700) | v);
        break;
    case 0x4003:
    case 0x4007:
        p = &a->pulse[(addr >> 2) & 1];
        p->period = (uint16_t)((p->period & 0xFF) | ((v & 7) << 8));
        if (p->enabled)
            p->length = kLength[v >> 3];
        p->step = 0;
        p->env.start = 1;
        break;
    case 0x4008:
        a->tri.control = (uint8_t)(v >> 7);
        a->tri.linear_load = (uint8_t)(v & 0x7F);
        break;
    case 0x400A:
        a->tri.period = (uint16_t)((a->tri.period & 0x700) | v);
        break;
    case 0x400B:
        a->tri.period = (uint16_t)((a->tri.period & 0xFF) | ((v & 7) << 8));
        if (a->tri.enabled)
            a->tri.length = kLength[v >> 3];
        a->tri.linear_reload = 1;
        break;
    case 0x400C:
        a->noise.env.loop = (uint8_t)((v >> 5) & 1);
        a->noise.env.constant = (uint8_t)((v >> 4) & 1);
        a->noise.env.volume = (uint8_t)(v & 15);
        break;
    case 0x400E:
        a->noise.mode = (uint8_t)(v >> 7);
        a->noise.period = kNoisePeriod[v & 15];
        break;
    case 0x400F:
        if (a->noise.enabled)
            a->noise.length = kLength[v >> 3];
        a->noise.env.start = 1;
        break;
    case 0x4010:
        a->dmc.irq_on = (uint8_t)(v >> 7);
        a->dmc.loop = (uint8_t)((v >> 6) & 1);
        a->dmc.period = kDmcPeriod[v & 15];
        if (!a->dmc.irq_on)
            a->dmc_irq = 0;
        break;
    case 0x4011:
        a->dmc.level = (uint8_t)(v & 0x7F);
        break;
    case 0x4012:
        a->dmc.start = (uint16_t)(0xC000 + v * 64);
        break;
    case 0x4013:
        a->dmc.length = (uint16_t)(v * 16 + 1);
        break;
    case 0x4015:
        a->pulse[0].enabled = (uint8_t)(v & 1);
        a->pulse[1].enabled = (uint8_t)((v >> 1) & 1);
        a->tri.enabled = (uint8_t)((v >> 2) & 1);
        a->noise.enabled = (uint8_t)((v >> 3) & 1);
        if (!a->pulse[0].enabled)
            a->pulse[0].length = 0;
        if (!a->pulse[1].enabled)
            a->pulse[1].length = 0;
        if (!a->tri.enabled)
            a->tri.length = 0;
        if (!a->noise.enabled)
            a->noise.length = 0;
        a->dmc_irq = 0;
        if (!(v & 0x10))
            a->dmc.remaining = 0;
        else if (a->dmc.remaining == 0)
        {
            a->dmc.address = a->dmc.start;
            a->dmc.remaining = a->dmc.length;
            dmc_fetch(a);
        }
        break;
    case 0x4017:
        a->five_step = (uint8_t)(v >> 7);
        a->irq_inhibit = (uint8_t)((v >> 6) & 1);
        if (a->irq_inhibit)
            a->frame_irq = 0;
        a->frame_cycle = 0;
        if (a->five_step)
        {
            quarter_frame(a);
            half_frame(a);
        }
        break;
    default:
        break;
    }
}

uint8_t apu_read_status(Apu *a)
{
    uint8_t v = (uint8_t)((a->pulse[0].length ? 0x01 : 0) | (a->pulse[1].length ? 0x02 : 0) |
                          (a->tri.length ? 0x04 : 0) | (a->noise.length ? 0x08 : 0) |
                          (a->dmc.remaining ? 0x10 : 0) | (a->frame_irq ? 0x40 : 0) |
                          (a->dmc_irq ? 0x80 : 0));

    a->frame_irq = 0;
    return v;
}

int apu_irq(const Apu *a)
{
    return a->frame_irq || a->dmc_irq;
}

/* ------------------------------------------------------------- running */

void apu_reset(Apu *a, uint32_t rate)
{
    int i;

    if (sPulseMix[1] == 0.0f)
    {
        for (i = 1; i < 31; i++)
            sPulseMix[i] = 95.52f / (8128.0f / (float)i + 100.0f);
        for (i = 1; i < 203; i++)
            sTndMix[i] = 163.67f / (24329.0f / (float)i + 100.0f);
    }
    memset(a, 0, sizeof(*a));
    a->rate = rate;
    a->pulse[0].ones_complement = 1;
    a->noise.lfsr = 1;
    a->noise.period = kNoisePeriod[0];
    a->dmc.period = kDmcPeriod[0];
    a->dmc.timer = a->dmc.period;
    a->dmc.bits = 8;
    a->dmc.silent = 1;
}

void apu_begin(Apu *a, int16_t *out, int max)
{
    a->out = out;
    a->out_n = 0;
    a->out_max = max;
}

static void emit(Apu *a)
{
    /* the console's output stage: two high-passes and a low-pass, here at
     * the output rate */
    const float dt = 1.0f / (float)a->rate;
    const float k90 = 1.0f / (1.0f + 2.0f * 3.14159265f * 90.0f * dt);
    const float k440 = 1.0f / (1.0f + 2.0f * 3.14159265f * 440.0f * dt);
    const float klp = (2.0f * 3.14159265f * 14000.0f * dt) / (1.0f + 2.0f * 3.14159265f * 14000.0f * dt);
    float x = a->acc / (float)a->acc_n, y;
    int s;

    a->acc = 0.0f;
    a->acc_n = 0;
    a->hp90_out = k90 * (a->hp90_out + x - a->hp90_prev_in);
    a->hp90_prev_in = x;
    a->hp440_out = k440 * (a->hp440_out + a->hp90_out - a->hp440_prev_in);
    a->hp440_prev_in = a->hp90_out;
    a->lp_out += klp * (a->hp440_out - a->lp_out);
    y = a->lp_out * 60000.0f;
    s = (int)(y < 0 ? y - 0.5f : y + 0.5f);
    if (s > 32767)
        s = 32767;
    else if (s < -32768)
        s = -32768;
    if (a->out && a->out_n < a->out_max)
        a->out[a->out_n++] = (int16_t)s;
}

void apu_run(Apu *a, int cycles)
{
    while (cycles-- > 0)
    {
        ApuTriangle *t = &a->tri;
        unsigned p1 = 0, p2 = 0, tri, noise = 0;
        int i;

        frame_counter_clock(a);

        /* the triangle every CPU cycle; it holds its step when either
         * counter is out */
        if (t->timer)
            t->timer--;
        else
        {
            t->timer = t->period;
            if (t->length && t->linear)
                t->step = (uint8_t)((t->step + 1) & 31);
        }

        /* the pulses every other */
        a->odd ^= 1;
        if (a->odd)
            for (i = 0; i < 2; i++)
            {
                ApuPulse *p = &a->pulse[i];

                if (p->timer)
                    p->timer--;
                else
                {
                    p->timer = p->period;
                    p->step = (uint8_t)((p->step - 1) & 7);
                }
            }

        /* the noise, its periods in CPU cycles */
        if (a->noise.timer > 1)
            a->noise.timer--;
        else
        {
            uint16_t l = a->noise.lfsr;
            uint16_t fb = (uint16_t)((l ^ (l >> (a->noise.mode ? 6 : 1))) & 1);

            a->noise.timer = a->noise.period;
            a->noise.lfsr = (uint16_t)((l >> 1) | (fb << 14));
        }

        dmc_clock(a);

        if (a->pulse[0].length && !pulse_muted(&a->pulse[0]) && kDuty[a->pulse[0].duty][a->pulse[0].step])
            p1 = envelope_out(&a->pulse[0].env);
        if (a->pulse[1].length && !pulse_muted(&a->pulse[1]) && kDuty[a->pulse[1].duty][a->pulse[1].step])
            p2 = envelope_out(&a->pulse[1].env);
        /* the triangle at an ultrasonic period is held at its middle, not
         * left to whine */
        tri = t->period < 2 ? 7 : kTriangle[t->step];
        if (a->noise.length && !(a->noise.lfsr & 1))
            noise = envelope_out(&a->noise.env);

        a->acc += sPulseMix[p1 + p2] + sTndMix[3 * tri + 2 * noise + a->dmc.level];
        a->acc_n++;
        a->phase += a->rate;
        if (a->phase >= APU_CPU_HZ)
        {
            a->phase -= APU_CPU_HZ;
            emit(a);
        }
    }
}
