/* ppu.c -- the 2C02 (ppu.h). */
#include "ppu.h"

#include <string.h>

/* tools/palette.py: the 2C02's composite signal, decoded */
const uint32_t ppu_rgb[64] = {
    0x666666, 0x002971, 0x181789, 0x360886, 0x4F0169, 0x5D0339, 0x5B0E04, 0x4B1F00,
    0x303100, 0x124000, 0x004700, 0x00450F, 0x003A44, 0x000000, 0x000000, 0x000000,
    0xAEAEAE, 0x245EBD, 0x4745DD, 0x6F32D9, 0x9028B3, 0xA22B74, 0xA03A2D, 0x8B5100,
    0x676900, 0x3F7D00, 0x1E8600, 0x0C833B, 0x0E7482, 0x000000, 0x000000, 0x000000,
    0xFFFFFF, 0x73ADFF, 0x9795FF, 0xBF81FF, 0xE177FF, 0xF37AC4, 0xF1897C, 0xDBA040,
    0xB7B91F, 0x8FCD23, 0x6DD64A, 0x5BD38A, 0x5DC4D2, 0x4E4E4E, 0x000000, 0x000000,
    0xFFFFFF, 0xC5DDFF, 0xD4D3FF, 0xE5CBFF, 0xF2C7FF, 0xFAC8E6, 0xF9CFC9, 0xF0D8B0,
    0xE1E2A3, 0xD1EAA4, 0xC3EEB4, 0xBBEDCF, 0xBCE7EC, 0xB6B6B6, 0x000000, 0x000000,
};

#define PRE_RENDER 261
#define VBLANK_LINE 241

#define CTRL_INC32 0x04
#define CTRL_SPR_TABLE 0x08
#define CTRL_BG_TABLE 0x10
#define CTRL_SPR16 0x20
#define CTRL_NMI 0x80
#define MASK_GRAY 0x01
#define MASK_BG_LEFT 0x02
#define MASK_SPR_LEFT 0x04
#define MASK_BG 0x08
#define MASK_SPR 0x10
#define STATUS_OVERFLOW 0x20
#define STATUS_HIT 0x40
#define STATUS_VBLANK 0x80

static inline int rendering(const Ppu *p)
{
    return (p->mask & (MASK_BG | MASK_SPR)) != 0;
}

static inline unsigned nt_index(const Ppu *p, uint16_t a)
{
    return p->mirror_vertical ? (a & 0x07FFu) : (((a >> 1) & 0x0400u) | (a & 0x03FFu));
}

static inline unsigned pal_index(uint16_t a)
{
    unsigned i = a & 0x1Fu;

    /* the sprite palettes' entry 0 is the background's */
    return (i & 0x13u) == 0x10u ? i & 0x0Fu : i;
}

static uint8_t vram_read(const Ppu *p, uint16_t a)
{
    a &= 0x3FFF;
    if (a < 0x2000)
        return p->chr[a];
    if (a < 0x3F00)
        return p->nt[nt_index(p, a)];
    return p->pal[pal_index(a)];
}

static void vram_write(Ppu *p, uint16_t a, uint8_t v)
{
    a &= 0x3FFF;
    if (a < 0x2000)
        p->chr[a] = v;
    else if (a < 0x3F00)
        p->nt[nt_index(p, a)] = v;
    else
        p->pal[pal_index(a)] = (uint8_t)(v & 0x3F);
}

void ppu_reset(Ppu *p, uint32_t *fb, int vertical)
{
    int keep = p->unlimited_sprites;

    memset(p, 0, sizeof(*p));
    p->unlimited_sprites = keep;
    p->fb = fb;
    p->mirror_vertical = (uint8_t)(vertical != 0);
    p->line = 0;
    p->dot = 0;
}

uint32_t ppu_backdrop(const Ppu *p)
{
    return 0xFF000000u | ppu_rgb[p->pal[0] & 0x3F];
}

/* ----------------------------------------------------------- the scroll */

static void inc_y(Ppu *p)
{
    if ((p->v & 0x7000) != 0x7000)
    {
        p->v += 0x1000;
        return;
    }
    p->v &= (uint16_t)~0x7000;
    {
        int y = (p->v & 0x03E0) >> 5;

        if (y == 29)
        {
            y = 0;
            p->v ^= 0x0800;
        }
        else if (y == 31)
            y = 0;
        else
            y++;
        p->v = (uint16_t)((p->v & ~0x03E0) | (y << 5));
    }
}

/* ------------------------------------------------------------- a line */

static void render_line(Ppu *p, int y)
{
    uint8_t bg[PPU_W + 16], spr[PPU_W], spr_front[PPU_W], spr_zero[PPU_W];
    uint32_t *out = p->fb + y * PPU_W;
    uint8_t gray = (p->mask & MASK_GRAY) ? 0x30 : 0x3F;
    int x, i, h, found;

    if (!rendering(p))
    {
        /* the backdrop -- or, the way the hardware shows it, the palette
         * entry v points at while it does */
        uint32_t c = ppu_rgb[(((p->v & 0x3F00) == 0x3F00) ? p->pal[pal_index(p->v)] : p->pal[0]) & gray];

        for (x = 0; x < PPU_W; x++)
            out[x] = 0xFF000000u | c;
        return;
    }

    /* the background: 33 tiles from v, the first shifted left by fine x */
    memset(bg, 0, sizeof(bg));
    if (p->mask & MASK_BG)
    {
        uint16_t v = p->v;
        unsigned fine_y = (v >> 12) & 7u, table = (p->ctrl & CTRL_BG_TABLE) ? 0x1000u : 0u;
        int t;

        for (t = 0; t < 33; t++)
        {
            uint8_t tile = p->nt[nt_index(p, (uint16_t)(0x2000 | (v & 0x0FFF)))];
            uint8_t attr = p->nt[nt_index(p, (uint16_t)(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07)))];
            unsigned shift = ((v >> 4) & 4u) | (v & 2u);
            uint8_t hi_pal = (uint8_t)(((attr >> shift) & 3u) << 2);
            const uint8_t *pat = p->chr + table + tile * 16u + fine_y;
            uint8_t lo = pat[0], hi = pat[8];
            uint8_t *d = bg + t * 8;
            int b;

            for (b = 0; b < 8; b++)
            {
                uint8_t c = (uint8_t)(((lo >> (7 - b)) & 1) | (((hi >> (7 - b)) & 1) << 1));

                d[b] = c ? (uint8_t)(hi_pal | c) : 0;
            }
            /* the next tile, across into the next nametable at the edge */
            if ((v & 0x001F) == 31)
                v = (uint16_t)((v & ~0x001F) ^ 0x0400);
            else
                v++;
        }
        if (p->x)
            memmove(bg, bg + p->x, PPU_W);
        if (!(p->mask & MASK_BG_LEFT))
            memset(bg, 0, 8);
    }

    /* the sprites: the first eight in OAM on this line (all of them, with
     * unlimited_sprites); the lower OAM index wins where two overlap */
    memset(spr, 0, sizeof(spr));
    if (p->mask & MASK_SPR)
    {
        h = (p->ctrl & CTRL_SPR16) ? 16 : 8;
        found = 0;
        for (i = 0; i < 64; i++)
        {
            const uint8_t *o = p->oam + i * 4;
            int row = y - (o[0] + 1), sx;
            unsigned addr;
            uint8_t lo, hi, at = o[2];

            if (row < 0 || row >= h)
                continue;
            if (++found > 8)
            {
                p->status |= STATUS_OVERFLOW;
                if (!p->unlimited_sprites)
                    break;
            }
            if (at & 0x80)
                row = h - 1 - row;
            if (h == 16)
                addr = ((o[1] & 1u) ? 0x1000u : 0u) + (o[1] & 0xFEu) * 16u + (row >= 8 ? 16u : 0u) + (row & 7u);
            else
                addr = ((p->ctrl & CTRL_SPR_TABLE) ? 0x1000u : 0u) + o[1] * 16u + (unsigned)row;
            lo = p->chr[addr];
            hi = p->chr[addr + 8];
            for (sx = 0; sx < 8; sx++)
            {
                int px = o[3] + sx, b = (at & 0x40) ? sx : 7 - sx;
                uint8_t c;

                if (px >= PPU_W || spr[px])
                    continue;
                c = (uint8_t)(((lo >> b) & 1) | (((hi >> b) & 1) << 1));
                if (!c)
                    continue;
                spr[px] = (uint8_t)(0x10 | ((at & 3) << 2) | c);
                spr_front[px] = (uint8_t)!(at & 0x20);
                spr_zero[px] = (uint8_t)(i == 0);
            }
        }
        if (!(p->mask & MASK_SPR_LEFT))
            memset(spr, 0, 8);
    }

    for (x = 0; x < PPU_W; x++)
    {
        uint8_t b = bg[x], s = spr[x], c;

        if (s)
        {
            if (b && spr_zero[x] && x != 255)
                p->status |= STATUS_HIT;
            c = (b && !spr_front[x]) ? b : s;
        }
        else
            c = b;
        out[x] = 0xFF000000u | ppu_rgb[p->pal[pal_index(c)] & gray];
    }
}

/* -------------------------------------------------------------- timing */

static int line_length(const Ppu *p)
{
    return (p->line == PRE_RENDER && p->odd && rendering(p)) ? PPU_DOTS - 1 : PPU_DOTS;
}

/* the next dot on this line at which something happens */
static int next_event(const Ppu *p)
{
    static const int kDots[] = { 1, 256, 257, 280 };
    int i;

    for (i = 0; i < 4; i++)
        if (kDots[i] > p->dot)
            return kDots[i];
    return line_length(p);
}

static void event(Ppu *p)
{
    int visible = p->line < PPU_H, pre = p->line == PRE_RENDER;

    switch (p->dot)
    {
    case 1:
        if (p->line == VBLANK_LINE)
        {
            p->status |= STATUS_VBLANK;
            if (p->ctrl & CTRL_NMI)
                p->nmi_out = 1;
            p->frame_done = 1;
        }
        else if (pre)
            p->status &= (uint8_t)~(STATUS_VBLANK | STATUS_HIT | STATUS_OVERFLOW);
        break;
    case 256:
        if (visible)
            render_line(p, p->line);
        if ((visible || pre) && rendering(p))
            inc_y(p);
        break;
    case 257:
        if ((visible || pre) && rendering(p))
            p->v = (uint16_t)((p->v & ~0x041F) | (p->t & 0x041F));
        break;
    case 280:
        if (pre && rendering(p))
            p->v = (uint16_t)((p->v & ~0x7BE0) | (p->t & 0x7BE0));
        break;
    default:
        /* the end of the line */
        p->dot = 0;
        if (++p->line == PPU_LINES)
        {
            p->line = 0;
            p->odd ^= 1;
            p->frame++;
        }
        break;
    }
}

void ppu_run(Ppu *p, int dots)
{
    p->budget += dots;
    while (p->budget > 0)
    {
        int step = next_event(p) - p->dot;

        if (step > p->budget)
        {
            p->dot += p->budget;
            p->budget = 0;
            break;
        }
        p->budget -= step;
        p->dot += step;
        event(p);
    }
}

/* ----------------------------------------------------------- registers */

uint8_t ppu_read_reg(Ppu *p, uint16_t addr)
{
    uint8_t v;

    switch (addr & 7)
    {
    case 2:
        v = (uint8_t)((p->status & 0xE0) | (p->latch & 0x1F));
        p->status &= (uint8_t)~STATUS_VBLANK;
        p->w = 0;
        p->latch = v;
        return v;
    case 4:
        v = p->oam[p->oam_addr];
        if ((p->oam_addr & 3) == 2)
            v &= 0xE3;          /* attribute bits 2-4 do not exist */
        p->latch = v;
        return v;
    case 7:
        if ((p->v & 0x3FFF) < 0x3F00)
        {
            v = p->read_buffer;
            p->read_buffer = vram_read(p, p->v);
        }
        else
        {
            /* palette reads are not delayed; the buffer takes the
             * nametable under them */
            v = (uint8_t)((vram_read(p, p->v) & 0x3F) | (p->latch & 0xC0));
            p->read_buffer = vram_read(p, (uint16_t)(p->v - 0x1000));
        }
        p->v = (uint16_t)((p->v + ((p->ctrl & CTRL_INC32) ? 32 : 1)) & 0x7FFF);
        p->latch = v;
        return v;
    default:
        return p->latch;        /* write-only */
    }
}

void ppu_write_reg(Ppu *p, uint16_t addr, uint8_t v)
{
    p->latch = v;
    switch (addr & 7)
    {
    case 0:
        /* NMI enabled during the vertical blank: one at once */
        if (!(p->ctrl & CTRL_NMI) && (v & CTRL_NMI) && (p->status & STATUS_VBLANK))
            p->nmi_out = 1;
        p->ctrl = v;
        p->t = (uint16_t)((p->t & ~0x0C00) | ((v & 3) << 10));
        break;
    case 1:
        p->mask = v;
        break;
    case 3:
        p->oam_addr = v;
        break;
    case 4:
        p->oam[p->oam_addr++] = v;
        break;
    case 5:
        if (!p->w)
        {
            p->t = (uint16_t)((p->t & ~0x001F) | (v >> 3));
            p->x = (uint8_t)(v & 7);
        }
        else
            p->t = (uint16_t)((p->t & ~0x73E0) | ((v & 7) << 12) | ((v & 0xF8) << 2));
        p->w ^= 1;
        break;
    case 6:
        if (!p->w)
            p->t = (uint16_t)((p->t & 0x00FF) | ((v & 0x3F) << 8));
        else
        {
            p->t = (uint16_t)((p->t & 0xFF00) | v);
            p->v = p->t;
        }
        p->w ^= 1;
        break;
    case 7:
        vram_write(p, p->v, v);
        p->v = (uint16_t)((p->v + ((p->ctrl & CTRL_INC32) ? 32 : 1)) & 0x7FFF);
        break;
    default:
        break;
    }
}

void ppu_oam_dma(Ppu *p, const uint8_t *page)
{
    int i;

    for (i = 0; i < 256; i++)
        p->oam[(uint8_t)(p->oam_addr + i)] = page[i];
}
