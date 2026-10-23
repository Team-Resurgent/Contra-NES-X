/* nes.h -- the NES, as much of it as Contra needs: the machine the Xbox
 * side and tools/hosttest drive.
 *
 *   cpu6502.c   the 2A03's 6502
 *   ppu.c       the 2C02: the picture
 *   apu.c       the 2A03's APU: the sound
 *   nes.c       the bus, the cartridge (UxROM, mapper 2: Contra's), the
 *               controllers, OAM DMA, a frame at a time, save states
 *
 * The cartridge is an iNES file: Contra's is 128 KB of PRG-ROM in eight
 * 16 KB banks, the last fixed at $C000 and one of the others switched in at
 * $8000 by a write anywhere in $8000-$FFFF (ANDed with the ROM's byte
 * there: the board's bus conflict), and 8 KB of CHR-RAM. Other mapper 2
 * games and NROM (mapper 0) cartridges load too; nothing else. */
#ifndef CNX_NES_H
#define CNX_NES_H

#include <stddef.h>
#include <stdint.h>

#include "apu.h"
#include "cpu6502.h"
#include "ppu.h"

#define NES_W PPU_W
#define NES_H PPU_H

/* a controller's buttons, in the order the pad shifts them out */
#define PAD_A      0x01
#define PAD_B      0x02
#define PAD_SELECT 0x04
#define PAD_START  0x08
#define PAD_UP     0x10
#define PAD_DOWN   0x20
#define PAD_LEFT   0x40
#define PAD_RIGHT  0x80

/* Contra's RAM, from the disassembly's ram.asm */
#define CONTRA_GAME_ROUTINE 0x18    /* GAME_ROUTINE_INDEX: 0-2 intro and title, 3+ a game */
#define CONTRA_DEMO_MODE    0x1C    /* 1 while the demo plays */
#define CONTRA_PAUSE        0x25    /* PAUSE_STATE: 1 while paused */
#define CONTRA_LEVEL        0x30    /* CURRENT_LEVEL: 0-7 */
#define CONTRA_P1_LIVES     0x32
#define CONTRA_P2_LIVES     0x33

typedef struct Nes
{
    Cpu6502 cpu;
    Ppu ppu;
    Apu apu;
    uint8_t ram[0x800];
    uint8_t bank;               /* the bank at $8000 */
    uint8_t pads[2];            /* the buttons held (PAD_*) */
    uint8_t shift[2];           /* the pads' shift registers */
    uint8_t strobe;
    uint8_t open_bus;
    int stall;                  /* CPU cycles owed to OAM DMA */
    uint32_t frames;

    /* the cartridge (not in a save state) */
    const uint8_t *prg;
    uint32_t prg_size;
    uint32_t prg_crc;
    uint8_t mapper;
    void (*log)(const char *msg);
} Nes;

/* 0, or -1 (and why, through log) when `rom` is not a cartridge this NES
 * takes. `rom` stays the caller's and must outlive the machine. */
int nes_init(Nes *n, const uint8_t *rom, size_t len, uint32_t *fb, uint32_t audio_rate);
void nes_reset(Nes *n);
/* one frame (to the next vertical blank): its picture in fb, its sound
 * (mono, audio_rate) in `audio`; the number of samples */
int nes_frame(Nes *n, int16_t *audio, int audio_max);

/* save states: the whole machine, for this cartridge only */
size_t nes_state_size(void);
void nes_state_save(const Nes *n, uint8_t *buf);
/* 0, or -1 when `buf` is not a state of this cartridge and this build */
int nes_state_load(Nes *n, const uint8_t *buf, size_t len);

#endif /* CNX_NES_H */
