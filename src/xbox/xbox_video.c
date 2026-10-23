/* xbox_video.c -- the NES picture on the television.
 *
 * The mode is the dashboard's: 720p when the HD pack is in and 720p is
 * ticked, else 480 (progressive when 480p is ticked), PAL at 60 Hz when the
 * console allows it, else 50. The NES's 256 x 224 that a television shows
 * is scaled by a whole number -- 2 at 480 lines, 3 at 720 -- so every NES
 * pixel is the same size, centred, and the rest of the screen is the NES's
 * backdrop colour, as its overscan is on a television. Point sampled
 * (sharp) by default; the left stick's click toggles smoothing.
 *
 * Two linear textures take turns: the one written this frame is not the one
 * the GPU may still be reading from the last. Linear textures are addressed
 * in texels, not 0..1. */
#include "xbox_xtl.h"

#include <string.h>

#include "xbox_port.h"

typedef struct
{
    float x, y, z, rhw;
    float u, v;
} Vtx;

#define VTX_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static LPDIRECT3D8 sD3D;
static LPDIRECT3DDEVICE8 sDev;
static LPDIRECT3DTEXTURE8 sTex[2];
static int sCur;
static int sW, sH, sScale, sWide480, sSmooth;

int xb_video_init(int *refresh_hz)
{
    D3DPRESENT_PARAMETERS pp;
    DWORD vflags = XGetVideoFlags();
    int want720 = XGetAVPack() == XC_AV_PACK_HDTV && (vflags & XC_VIDEO_FLAGS_HDTV_720p);
    int pal = XGetVideoStandard() == XC_VIDEO_STANDARD_PAL_I;
    HRESULT hr;
    int i;

    sD3D = Direct3DCreate8(D3D_SDK_VERSION);
    if (sD3D == NULL)
        return -1;
    *refresh_hz = (pal && !(vflags & XC_VIDEO_FLAGS_PAL_60Hz)) ? 50 : 60;
    for (;;)
    {
        memset(&pp, 0, sizeof(pp));
        if (want720)
        {
            sW = 1280;
            sH = 720;
            pp.Flags = D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN;
        }
        else
        {
            sW = 640;
            sH = 480;
            if (vflags & XC_VIDEO_FLAGS_HDTV_480p)
                pp.Flags = D3DPRESENTFLAG_PROGRESSIVE;
            if (vflags & XC_VIDEO_FLAGS_WIDESCREEN)
                pp.Flags |= D3DPRESENTFLAG_WIDESCREEN;
        }
        sWide480 = !want720 && (vflags & XC_VIDEO_FLAGS_WIDESCREEN);
        pp.BackBufferWidth = (UINT)sW;
        pp.BackBufferHeight = (UINT)sH;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.EnableAutoDepthStencil = FALSE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.FullScreen_RefreshRateInHz = (UINT)*refresh_hz;
        pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        hr = IDirect3D8_CreateDevice(sD3D, 0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &pp, &sDev);
        if (SUCCEEDED(hr) || !want720)
            break;
        xb_log("video: 720p failed (%08lx); 480 instead", (unsigned long)hr);
        want720 = 0;
    }
    if (FAILED(hr))
        return -1;
    sScale = sH >= 720 ? 3 : 2;

    for (i = 0; i < 2; i++)
        if (FAILED(IDirect3DDevice8_CreateTexture(sDev, NES_W, NES_H, 1, 0, D3DFMT_LIN_X8R8G8B8,
                                                  D3DPOOL_DEFAULT, &sTex[i])))
            return -1;

    IDirect3DDevice8_SetVertexShader(sDev, VTX_FVF);
    IDirect3DDevice8_SetRenderState(sDev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(sDev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice8_SetRenderState(sDev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(sDev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(sDev, D3DRS_DITHERENABLE, FALSE);
    IDirect3DDevice8_SetTextureStageState(sDev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(sDev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(sDev, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice8_SetTextureStageState(sDev, 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice8_SetTextureStageState(sDev, 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice8_SetTextureStageState(sDev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice8_SetTextureStageState(sDev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    IDirect3DDevice8_Clear(sDev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    IDirect3DDevice8_Present(sDev, NULL, NULL, NULL, NULL);
    xb_log("video: %dx%d%s at %d Hz, the picture x%d", sW, sH,
           (pp.Flags & D3DPRESENTFLAG_PROGRESSIVE) ? "p" : "i", *refresh_hz, sScale);
    return 0;
}

void xb_video_toggle_smooth(void)
{
    sSmooth ^= 1;
}

void xb_video_present(const uint32_t *fb, int top, int lines, uint32_t border)
{
    LPDIRECT3DTEXTURE8 tex;
    D3DLOCKED_RECT lr;
    Vtx v[4];
    float w, h, x0, y0, v0, v1;
    int y;

    if (sDev == NULL)
        return;
    if (top < 0 || top + lines > NES_H)
    {
        top = 0;
        lines = NES_H;
    }
    tex = sTex[sCur];
    sCur ^= 1;
    if (SUCCEEDED(IDirect3DTexture8_LockRect(tex, 0, &lr, NULL, 0)))
    {
        for (y = 0; y < NES_H; y++)
            memcpy((uint8_t *)lr.pBits + y * lr.Pitch, fb + y * NES_W, NES_W * 4);
        IDirect3DTexture8_UnlockRect(tex, 0);
    }

    /* whole-number scale; a 16:9 television sent 480 lines stretches them
     * by 4:3, which a quarter off the width undoes */
    w = (float)(NES_W * sScale) * (sWide480 ? 0.75f : 1.0f);
    h = (float)(lines * sScale);
    x0 = (float)(int)((sW - w) / 2) - 0.5f;
    y0 = (float)(int)((sH - h) / 2) - 0.5f;
    v0 = (float)top;
    v1 = (float)(top + lines);
    v[0].x = x0;     v[0].y = y0;     v[0].u = 0.0f;         v[0].v = v0;
    v[1].x = x0 + w; v[1].y = y0;     v[1].u = (float)NES_W; v[1].v = v0;
    v[2].x = x0;     v[2].y = y0 + h; v[2].u = 0.0f;         v[2].v = v1;
    v[3].x = x0 + w; v[3].y = y0 + h; v[3].u = (float)NES_W; v[3].v = v1;
    for (y = 0; y < 4; y++)
    {
        v[y].z = 0.0f;
        v[y].rhw = 1.0f;
    }

    IDirect3DDevice8_Clear(sDev, 0, NULL, D3DCLEAR_TARGET, (D3DCOLOR)(border | 0xFF000000u), 1.0f, 0);
    IDirect3DDevice8_BeginScene(sDev);
    IDirect3DDevice8_SetTexture(sDev, 0, (D3DBaseTexture *)tex);
    IDirect3DDevice8_SetTextureStageState(sDev, 0, D3DTSS_MINFILTER, sSmooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(sDev, 0, D3DTSS_MAGFILTER, sSmooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    IDirect3DDevice8_DrawPrimitiveUP(sDev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
    IDirect3DDevice8_EndScene(sDev);
    IDirect3DDevice8_SetTexture(sDev, 0, NULL);
    IDirect3DDevice8_Present(sDev, NULL, NULL, NULL, NULL);
}
