/* nes.c -- the machine (nes.h). */
#include "nes.h"

#include <stdio.h>
#include <string.h>

#define INES_HEADER 16
#define STATE_MAGIC 0x53584E43u     /* "CNXS" */
#define STATE_VERSION 1

typedef struct StateHeader
{
    uint32_t magic, version, size, prg_crc;
} StateHeader;

static void say(Nes *n, const char *fmt, unsigned a, unsigned b)
{
    char line[160];

    if (!n->log)
        return;
    snprintf(line, sizeof(line), fmt, a, b);
    n->log(line);
}

static uint32_t crc32_of(const uint8_t *p, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;

    for (i = 0; i < len; i++)
    {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

/* ------------------------------------------------------------- the bus */

static inline uint8_t prg_read(const Nes *n, uint16_t a)
{
    uint32_t off;

    if (a >= 0xC000)
        off = n->prg_size - 0x4000u + (a - 0xC000u);
    else
        off = (uint32_t)n->bank * 0x4000u + (a - 0x8000u);
    return n->prg[off % n->prg_size];
}

static uint8_t bus_read(void *ctx, uint16_t a)
{
    Nes *n = (Nes *)ctx;
    uint8_t v;

    if (a < 0x2000)
        v = n->ram[a & 0x7FF];
    else if (a < 0x4000)
        v = ppu_read_reg(&n->ppu, a);
    else if (a == 0x4015)
        v = (uint8_t)(apu_read_status(&n->apu) | (n->open_bus & 0x20));
    else if (a == 0x4016 || a == 0x4017)
    {
        int i = a & 1;

        /* the pad's next button in bit 0; the rest is the bus. Once the
         * eight are out, a real pad gives ones */
        v = (uint8_t)((n->open_bus & 0xE0) | (n->strobe ? (n->pads[i] & 1) : (n->shift[i] & 1)));
        if (!n->strobe)
            n->shift[i] = (uint8_t)((n->shift[i] >> 1) | 0x80);
    }
    else if (a >= 0x8000)
        v = prg_read(n, a);
    else
        v = n->open_bus;        /* no PRG-RAM, no expansion */
    n->open_bus = v;
    return v;
}

static void bus_write(void *ctx, uint16_t a, uint8_t v)
{
    Nes *n = (Nes *)ctx;

    n->open_bus = v;
    if (a < 0x2000)
        n->ram[a & 0x7FF] = v;
    else if (a < 0x4000)
        ppu_write_reg(&n->ppu, a, v);
    else if (a == 0x4014)
    {
        uint8_t page[256];
        int i;

        for (i = 0; i < 256; i++)
            page[i] = bus_read(n, (uint16_t)((v << 8) | i));
        ppu_oam_dma(&n->ppu, page);
        n->stall += 513 + (int)(n->cpu.cycles & 1);
    }
    else if (a == 0x4016)
    {
        n->strobe = (uint8_t)(v & 1);
        if (n->strobe)
        {
            n->shift[0] = n->pads[0];
            n->shift[1] = n->pads[1];
        }
    }
    else if (a < 0x4018)
        apu_write(&n->apu, a, v);
    else if (a >= 0x8000 && n->mapper == 2)
        n->bank = (uint8_t)((v & prg_read(n, a)) % (n->prg_size / 0x4000u));
}

/* -------------------------------------------------------------- set up */

static void wire(Nes *n, uint32_t *fb)
{
    n->cpu.ctx = n;
    n->cpu.read = bus_read;
    n->cpu.write = bus_write;
    n->apu.ctx = n;
    n->apu.read = bus_read;
    n->ppu.fb = fb;
}

int nes_init(Nes *n, const uint8_t *rom, size_t len, uint32_t *fb, uint32_t audio_rate)
{
    void (*log)(const char *) = n->log;
    unsigned prg_banks, chr_banks, mapper;

    memset(n, 0, sizeof(*n));
    n->log = log;
    if (len < INES_HEADER || memcmp(rom, "NES\x1A", 4) != 0)
    {
        say(n, "nes: not an iNES file", 0, 0);
        return -1;
    }
    prg_banks = rom[4];
    chr_banks = rom[5];
    mapper = (unsigned)((rom[6] >> 4) | (rom[7] & 0xF0));
    if (mapper != 0 && mapper != 2)
    {
        say(n, "nes: mapper %u: only UxROM (2) and NROM (0) are built in", mapper, 0);
        return -1;
    }
    if (prg_banks == 0 || chr_banks > 1 || (rom[6] & 0x04) ||
        len < INES_HEADER + prg_banks * 0x4000u + chr_banks * 0x2000u)
    {
        say(n, "nes: %u PRG banks, %u CHR banks: not a cartridge this NES takes", prg_banks, chr_banks);
        return -1;
    }
    n->mapper = (uint8_t)mapper;
    n->prg = rom + INES_HEADER;
    n->prg_size = prg_banks * 0x4000u;
    n->prg_crc = crc32_of(n->prg, n->prg_size);
    ppu_reset(&n->ppu, fb, rom[6] & 1);
    if (chr_banks)
        memcpy(n->ppu.chr, rom + INES_HEADER + n->prg_size, 0x2000);
    apu_reset(&n->apu, audio_rate);
    wire(n, fb);
    nes_reset(n);
    return 0;
}

void nes_reset(Nes *n)
{
    int i;

    /* RAM comes up much as it powers up: not quite zero; Contra clears it */
    for (i = 0; i < 0x800; i++)
        n->ram[i] = (i & 4) ? 0xFF : 0x00;
    n->bank = 0;
    n->stall = 0;
    n->apu.frame_irq = n->apu.dmc_irq = 0;
    n->apu.dmc.remaining = 0;
    n->ppu.ctrl = n->ppu.mask = 0;
    n->ppu.w = 0;
    cpu_reset(&n->cpu);
}

/* ---------------------------------------------------------- a frame */

int nes_frame(Nes *n, int16_t *audio, int audio_max)
{
    apu_begin(&n->apu, audio, audio_max);
    n->ppu.frame_done = 0;
    while (!n->ppu.frame_done)
    {
        int cyc = cpu_step(&n->cpu);

        if (n->cpu.jammed)
        {
            if (n->cpu.jammed == 1)
                say(n, "nes: the CPU jammed at $%04X (opcode %02X)", n->cpu.pc,
                    n->cpu.pc >= 0x8000 ? prg_read(n, n->cpu.pc) : n->ram[n->cpu.pc & 0x7FF]);
            n->cpu.jammed = 2;
            /* the picture stops: run the PPU so frames still end */
            ppu_run(&n->ppu, 3 * 29781);
            n->ppu.frame_done = 1;
            break;
        }
        cyc += n->stall;
        n->cpu.cycles += (uint32_t)n->stall;
        n->stall = 0;
        apu_run(&n->apu, cyc);
        /* the DMC's reads hold the CPU, not the APU: it runs on through
         * them (and may read again) */
        while (n->apu.stall)
        {
            int s = n->apu.stall;

            n->apu.stall = 0;
            apu_run(&n->apu, s);
            cyc += s;
            n->cpu.cycles += (uint32_t)s;
        }
        ppu_run(&n->ppu, cyc * 3);
        if (n->ppu.nmi_out)
        {
            n->ppu.nmi_out = 0;
            n->cpu.nmi_pending = 1;
        }
        n->cpu.irq_line = apu_irq(&n->apu);
    }
    n->frames++;
    return n->apu.out_n;
}

/* --------------------------------------------------------- save states */

size_t nes_state_size(void)
{
    return sizeof(StateHeader) + sizeof(Nes);
}

void nes_state_save(const Nes *n, uint8_t *buf)
{
    StateHeader h;

    h.magic = STATE_MAGIC;
    h.version = STATE_VERSION;
    h.size = (uint32_t)sizeof(Nes);
    h.prg_crc = n->prg_crc;
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), n, sizeof(Nes));
}

int nes_state_load(Nes *n, const uint8_t *buf, size_t len)
{
    StateHeader h;
    Nes keep = *n;

    if (len != nes_state_size())
        return -1;
    memcpy(&h, buf, sizeof(h));
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION || h.size != sizeof(Nes) ||
        h.prg_crc != n->prg_crc)
        return -1;
    memcpy(n, buf + sizeof(h), sizeof(Nes));
    /* what belongs to this run, not to the state */
    n->prg = keep.prg;
    n->prg_size = keep.prg_size;
    n->prg_crc = keep.prg_crc;
    n->mapper = keep.mapper;
    n->log = keep.log;
    n->ppu.unlimited_sprites = keep.ppu.unlimited_sprites;
    n->apu.out = keep.apu.out;
    n->apu.out_n = keep.apu.out_n;
    n->apu.out_max = keep.apu.out_max;
    n->apu.rate = keep.apu.rate;
    wire(n, keep.ppu.fb);
    return 0;
}
