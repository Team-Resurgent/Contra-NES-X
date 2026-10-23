/* xbox_save.c -- the save state, kept on the hard disk.
 *
 * Contra on the NES saves nothing; the port keeps one state of the whole
 * machine (nes_state_save) when you ask, in the title's save game
 * "Contra NES X" as contra.sta: the dashboard lists it beside the game,
 * with the save icon, and can delete or copy it like any other. */
#include "xbox_xtl.h"

#include <stdlib.h>
#include <string.h>

#include "xbox_port.h"

#define STATE_FILE "contra.sta"
#define STATE_TMP "contra.tmp"
#define STATE_MAX (256 * 1024)

static char sDir[MAX_PATH];
static int sDirState;           /* 0 not tried, 1 there, -1 none */

static const char *save_dir(void)
{
    DWORD rv;
    size_t n;

    if (sDirState)
        return sDirState > 0 ? sDir : NULL;
    rv = XCreateSaveGame("U:\\", L"Contra NES X", OPEN_ALWAYS, 0, sDir, sizeof(sDir));
    if (rv != ERROR_SUCCESS)
    {
        xb_log("save: no save game (%lu)", (unsigned long)rv);
        sDirState = -1;
        return NULL;
    }
    n = strlen(sDir);
    if (n && sDir[n - 1] != '\\' && n + 1 < sizeof(sDir))
    {
        sDir[n] = '\\';
        sDir[n + 1] = '\0';
    }
    sDirState = 1;
    return sDir;
}

static int path_of(char *out, size_t size, const char *name)
{
    const char *d = save_dir();

    if (!d || strlen(d) + strlen(name) + 1 > size)
        return 0;
    strcpy(out, d);
    strcat(out, name);
    return 1;
}

uint8_t *xb_save_load(size_t *len)
{
    char path[MAX_PATH];
    HANDLE h;
    DWORD size, got = 0;
    uint8_t *d = NULL;

    *len = 0;
    if (sDirState < 0)
        sDirState = 0;          /* the dashboard could not make it at boot: try again */
    if (!path_of(path, sizeof(path), STATE_FILE))
        return NULL;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(h, NULL);
    if (size != INVALID_FILE_SIZE && size > 0 && size <= STATE_MAX)
        d = (uint8_t *)malloc(size);
    if (d && (!ReadFile(h, d, size, &got, NULL) || got != size))
    {
        free(d);
        d = NULL;
    }
    CloseHandle(h);
    if (d)
        *len = size;
    return d;
}

/* the save game's folder flushed too: the new folder and file entries,
 * not only the file's bytes, on the disk before anyone resets the console
 * (an in-game reset or the power button skips the cache's own flush) */
static void flush_dir(void)
{
    HANDLE h;

    if (sDirState <= 0)
        return;
    h = CreateFileA(sDir, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    FlushFileBuffers(h);
    CloseHandle(h);
}

/* nonzero when the state is on the disk. Written beside the old one and
 * then put in its place, so a failed write leaves the last good state. */
int xb_save_store(const uint8_t *data, size_t len)
{
    char path[MAX_PATH], tmp[MAX_PATH];
    HANDLE h;
    DWORD put = 0;
    int ok;

    if (sDirState < 0)
        sDirState = 0;
    if (!path_of(path, sizeof(path), STATE_FILE) || !path_of(tmp, sizeof(tmp), STATE_TMP))
        return 0;
    h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        xb_log("save: cannot write %s (%lu)", tmp, (unsigned long)GetLastError());
        return 0;
    }
    ok = WriteFile(h, data, (DWORD)len, &put, NULL) && put == len;
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (ok)
    {
        DeleteFileA(path);
        ok = MoveFileA(tmp, path) != 0;
    }
    if (!ok)
    {
        xb_log("save: %s was not written (%lu)", path, (unsigned long)GetLastError());
        DeleteFileA(tmp);
        return 0;
    }
    flush_dir();
    xb_log("save: the state, %u bytes, in %s", (unsigned)len, path);
    return 1;
}
