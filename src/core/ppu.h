/* ppu.h -- the NES's picture: the 2C02.
 *
 * The registers ($2000-$2007) as the CPU sees them, with the scroll
 * registers' "loopy" v / t / x / w; 8 KB of CHR-RAM (Contra's cartridge has
 * no CHR-ROM: the game decompresses its tiles into it), 2 KB of nametables
 * mirrored as the cartridge wires them, the 32 palette entries and the
 * 256-byte OAM.
 *
 * Timing is the NTSC PPU's: 341 dots by 262 lines, the vertical blank (and
 * the NMI) from line 241, the pre-render line's dot dropped on odd frames
 * while rendering. A visible line is drawn whole when the beam reaches its
 * dot 256, from the registers as they are then -- Contra changes nothing
 * mid-line -- background, then up to eight sprites (eight-by-sixteen or
 * eight-by-eight), sprite 0 hit and the overflow flag. `unlimited_sprites`
 * lifts the eight-a-line limit (no flicker). */
#ifndef CNX_PPU_H
#define CNX_PPU_H

#include <stdint.h>

#define PPU_W 256
#define PPU_H 240
#define PPU_LINES 262
#define PPU_DOTS 341

typedef struct Ppu
{
    uint8_t ctrl, mask, status, oam_addr;
    uint16_t v, t;
    uint8_t x, w;
    uint8_t read_buffer;        /* $2007's delayed read */
    uint8_t latch;              /* the registers' open bus */
    uint8_t mirror_vertical;
    uint8_t nmi_out;            /* an NMI to give the CPU (the machine clears it) */
    uint8_t frame_done;         /* the picture is whole: the vertical blank has begun */
    uint8_t odd;
    int line, dot;              /* line 261 is the pre-render line */
    int budget;                 /* dots owed */
    uint32_t frame;

    uint8_t nt[2048];
    uint8_t pal[32];
    uint8_t oam[256];
    uint8_t chr[8192];

    /* options (kept across a state load by the machine) */
    int unlimited_sprites;

    /* the picture, PPU_W x PPU_H, 0xAARRGGBB (a pointer: put back by the
     * machine after a state load) */
    uint32_t *fb;
} Ppu;

/* the NES's 64 colours, 0x00RRGGBB */
extern const uint32_t ppu_rgb[64];

void ppu_reset(Ppu *p, uint32_t *fb, int vertical);
/* run on `dots` dots */
void ppu_run(Ppu *p, int dots);
uint8_t ppu_read_reg(Ppu *p, uint16_t addr);
void ppu_write_reg(Ppu *p, uint16_t addr, uint8_t v);
/* OAM DMA: 256 bytes from the CPU's page, from OAMADDR on */
void ppu_oam_dma(Ppu *p, const uint8_t *page);
/* the backdrop colour (palette entry 0), 0xFFRRGGBB: the border round the picture */
uint32_t ppu_backdrop(const Ppu *p);

#endif /* CNX_PPU_H */
