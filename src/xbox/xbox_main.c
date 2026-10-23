/* xbox_main.c -- the Xbox entry point: Contra (NES, 1988) on the port's
 * NES (src/core), one NES frame per television frame.
 *
 * The cartridge is D:\data\Contra.nes, which setup.py assembles from the
 * disassembly (with your baserom.nes's graphics and music) and puts on the
 * disc. */
#include "xbox_xtl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/osd.h"
#include "xbox_port.h"

#define ROM_PATH "D:\\data\\Contra.nes"
/* the lines a television shows: the NES's top and bottom eight are under
 * the bezel on an NTSC set, and Contra keeps nothing there */
#define SHOW_TOP 8
#define SHOW_LINES 224
#define MESSAGE_FRAMES 120

static Nes sNes;
static uint32_t sFb[NES_W * NES_H];
static int16_t sAudio[XB_AUDIO_RATE / 50];
static Osd sOsd;

void xb_log(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n] = '\n';
    line[n + 1] = 0;
    OutputDebugStringA(line);
}

static void core_log(const char *msg)
{
    xb_log("%s", msg);
}

/* A fault's address, to the debug channel, before the console restarts:
 * look it up in out/<Config>/default.exe's symbols. */
static LONG WINAPI fault_report(struct _EXCEPTION_POINTERS *ep)
{
    xb_log("cnx: FAULT %08lx at %08lx", (unsigned long)ep->ExceptionRecord->ExceptionCode,
           (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    return 0;                   /* EXCEPTION_CONTINUE_SEARCH */
}

static uint8_t *read_whole(const char *path, size_t *len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    uint8_t *d;

    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(h, NULL);
    d = (size != INVALID_FILE_SIZE && size > 0) ? (uint8_t *)malloc(size) : NULL;
    if (d && (!ReadFile(h, d, size, &got, NULL) || got != size))
    {
        free(d);
        d = NULL;
    }
    CloseHandle(h);
    *len = d ? size : 0;
    return d;
}

/* no cartridge: a red screen for a while, then the dashboard (the debug
 * channel says why) */
static void give_up(const char *why)
{
    int i;

    xb_log("cnx: %s", why);
    for (i = 0; i < NES_W * NES_H; i++)
        sFb[i] = 0xFF600000u;
    for (i = 0; i < 60 * 8; i++)
        xb_video_present(sFb, SHOW_TOP, SHOW_LINES, 0xFF000000u);
    XLaunchNewImage(NULL, NULL);
}

/* a game is on (not the title, the intro or the demo) */
static int in_game(void)
{
    return sNes.ram[CONTRA_GAME_ROUTINE] >= 3 && !sNes.ram[CONTRA_DEMO_MODE];
}

static void save_state(void)
{
    size_t len = nes_state_size();
    uint8_t *buf;

    if (!in_game() || !sNes.ram[CONTRA_PAUSE])
    {
        osd_show(&sOsd, "PAUSE TO SAVE", MESSAGE_FRAMES);
        return;
    }
    buf = (uint8_t *)malloc(len);
    if (!buf)
        return;
    nes_state_save(&sNes, buf);
    osd_show(&sOsd, xb_save_store(buf, len) ? "STATE SAVED" : "SAVE FAILED", MESSAGE_FRAMES);
    free(buf);
}

static void load_state(void)
{
    size_t len;
    uint8_t *buf;

    if (in_game() && !sNes.ram[CONTRA_PAUSE])
    {
        osd_show(&sOsd, "PAUSE TO LOAD", MESSAGE_FRAMES);
        return;
    }
    buf = xb_save_load(&len);
    if (!buf)
    {
        osd_show(&sOsd, "NO SAVED STATE", MESSAGE_FRAMES);
        return;
    }
    if (nes_state_load(&sNes, buf, len) == 0)
        osd_show(&sOsd, "STATE LOADED - START TO PLAY", MESSAGE_FRAMES);
    else
        osd_show(&sOsd, "SAVED STATE NOT USABLE", MESSAGE_FRAMES);
    free(buf);
}

void __cdecl main(void)
{
    uint8_t *rom;
    size_t rom_len;
    int hz = 60, acc = 0;

    SetUnhandledExceptionFilter(fault_report);
    xb_input_init();            /* enumeration is asynchronous: start it first */
    if (xb_video_init(&hz) != 0)
    {
        xb_log("cnx: no Direct3D device");
        XLaunchNewImage(NULL, NULL);
    }

    rom = read_whole(ROM_PATH, &rom_len);
    if (!rom)
        give_up("no " ROM_PATH " -- run setup.py, which builds it into the disc");
    sNes.log = core_log;
    if (nes_init(&sNes, rom, rom_len, sFb, XB_AUDIO_RATE) != 0)
        give_up("Contra.nes is not a cartridge this NES takes");
    xb_log("cnx: Contra.nes, %u KB of PRG, CRC32 %08lx", (unsigned)(sNes.prg_size / 1024),
           (unsigned long)sNes.prg_crc);
    if (xb_audio_init() != 0)
        xb_log("cnx: no sound");

    for (;;)
    {
        unsigned cmd = xb_input_poll(sNes.pads);

        if (cmd & XB_CMD_QUIT)
            XLaunchNewImage(NULL, NULL);
        if (cmd & XB_CMD_SMOOTH)
            xb_video_toggle_smooth();
        if (cmd & XB_CMD_SPRITES)
        {
            sNes.ppu.unlimited_sprites ^= 1;
            osd_show(&sOsd, sNes.ppu.unlimited_sprites ? "ALL SPRITES - NO FLICKER" : "SPRITE LIMIT AS THE NES",
                     MESSAGE_FRAMES);
        }
        if (cmd & XB_CMD_SAVE)
            save_state();
        if (cmd & XB_CMD_LOAD)
            load_state();
        /* the NES's 60 frames a second, whatever the television's rate:
         * at 50 Hz six frames go by in five */
        for (acc += 60; acc >= hz; acc -= hz)
        {
            int n = nes_frame(&sNes, sAudio, (int)(sizeof(sAudio) / sizeof(sAudio[0])));

            xb_audio_push(sAudio, n);
        }
        osd_draw(&sOsd, sFb, NES_W, NES_H);
        xb_video_present(sFb, SHOW_TOP, SHOW_LINES, ppu_backdrop(&sNes.ppu));
    }
}
