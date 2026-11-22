// SPDX-License-Identifier: MIT
// libbc250 - user-mode API over BC250Display.sys (D3DKMTEscape).
//
// This is the "winsys" layer: the thin piece a user-mode driver (a Mesa RADV port, a
// compute runtime, a test) talks to. Today it exposes what the kernel driver implements:
// device discovery, adapter info, SDMA indirect-buffer submission with fences, VRAM
// scratch reads and the scanout fill demo. See docs/ROADMAP.md for what a full RADV
// winsys still needs (CP/compute queues, allocations, WDDM fences).
//
// Header-only; define BC250_IMPLEMENTATION in exactly one .c/.cpp file.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include "..\..\src\gpu\bc250_escape.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bc250_device {
    D3DKMT_HANDLE adapter;
    LUID          luid;
} bc250_device;

// Returns BC250_OK or a negative BC250_E_* code. Transport failures return BC250_E_NOHW.
int  bc250_open(bc250_device* dev);
void bc250_close(bc250_device* dev);
int  bc250_escape(bc250_device* dev, BC250_ESCAPE* e);       // e->Header.Command must be set
int  bc250_get_info(bc250_device* dev, BC250_INFO* out);
int  bc250_submit_ib(bc250_device* dev, const bc_u32* ib, bc_u32 count, bc_u32* fence, bc_u32* elapsed_us,
                     bc_u64* scratch_mc);
int  bc250_fill_rect(bc250_device* dev, bc_u32 x, bc_u32 y, bc_u32 w, bc_u32 h, bc_u32 xrgb);
int  bc250_vram_read(bc250_device* dev, bc_u32 offset, bc_u32 count, bc_u32* out);
const char* bc250_strerror(int status);

// SDMA v5 packet helpers (navi10_sdma_pkt_open.h)
#define BC250_SDMA_HDR(op, sub)        (((op) & 0xFFu) | (((sub) & 0xFFu) << 8))
#define BC250_SDMA_OP_NOP              0u
#define BC250_SDMA_OP_COPY             1u
#define BC250_SDMA_OP_WRITE            2u
#define BC250_SDMA_OP_FENCE            5u
#define BC250_SDMA_OP_CONST_FILL       11u

static inline bc_u32 bc250_emit_write(bc_u32* p, bc_u64 dst, const bc_u32* data, bc_u32 n)
{
    bc_u32 k = 0;
    p[k++] = BC250_SDMA_HDR(BC250_SDMA_OP_WRITE, 0);
    p[k++] = (bc_u32)dst; p[k++] = (bc_u32)(dst >> 32);
    p[k++] = n - 1;
    for (bc_u32 i = 0; i < n; ++i) p[k++] = data[i];
    return k;
}

static inline bc_u32 bc250_emit_copy(bc_u32* p, bc_u64 src, bc_u64 dst, bc_u32 bytes)
{
    p[0] = BC250_SDMA_HDR(BC250_SDMA_OP_COPY, 0);
    p[1] = bytes - 1; p[2] = 0;
    p[3] = (bc_u32)src; p[4] = (bc_u32)(src >> 32);
    p[5] = (bc_u32)dst; p[6] = (bc_u32)(dst >> 32);
    return 7;
}

static inline bc_u32 bc250_emit_fill(bc_u32* p, bc_u64 dst, bc_u32 value, bc_u32 bytes)
{
    p[0] = BC250_SDMA_HDR(BC250_SDMA_OP_CONST_FILL, 0) | (2u << 30);   // dword fill
    p[1] = (bc_u32)dst; p[2] = (bc_u32)(dst >> 32);
    p[3] = value; p[4] = bytes - 1;
    return 5;
}

#ifdef __cplusplus
}
#endif

// ------------------------------------------------------------------------------------------
#ifdef BC250_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "gdi32.lib")

static int bc250__raw(D3DKMT_HANDLE h, BC250_ESCAPE* e)
{
    e->Header.Magic = BC250_ESC_MAGIC;
    e->Header.Version = BC250_ESC_VERSION;
    D3DKMT_ESCAPE esc;
    memset(&esc, 0, sizeof(esc));
    esc.hAdapter = h;
    esc.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    esc.pPrivateDriverData = e;
    esc.PrivateDriverDataSize = sizeof(*e);
    return D3DKMTEscape(&esc) == 0 ? BC250_OK : BC250_E_NOHW;
}

int bc250_open(bc250_device* dev)
{
    memset(dev, 0, sizeof(*dev));
    D3DKMT_ENUMADAPTERS2 en;
    memset(&en, 0, sizeof(en));
    if (D3DKMTEnumAdapters2(&en) != 0 || !en.NumAdapters) return BC250_E_NOHW;
    D3DKMT_ADAPTERINFO* infos = (D3DKMT_ADAPTERINFO*)calloc(en.NumAdapters, sizeof(*infos));
    if (!infos) return BC250_E_NOMEM;
    en.pAdapters = infos;
    int found = 0;
    if (D3DKMTEnumAdapters2(&en) == 0)
    {
        BC250_ESCAPE* e = (BC250_ESCAPE*)calloc(1, sizeof(BC250_ESCAPE));
        for (ULONG i = 0; e && i < en.NumAdapters; ++i)
        {
            memset(e, 0, sizeof(*e));
            e->Header.Command = BC250_CMD_GET_INFO;
            int ok = !found && bc250__raw(infos[i].hAdapter, e) == BC250_OK &&
                     e->Header.Status == BC250_OK && e->u.Info.VendorId == 0x1002;
            if (ok) { dev->adapter = infos[i].hAdapter; dev->luid = infos[i].AdapterLuid; found = 1; }
            else { D3DKMT_CLOSEADAPTER c; c.hAdapter = infos[i].hAdapter; D3DKMTCloseAdapter(&c); }
        }
        free(e);
    }
    free(infos);
    return found ? BC250_OK : BC250_E_NOHW;
}

void bc250_close(bc250_device* dev)
{
    if (dev->adapter) { D3DKMT_CLOSEADAPTER c; c.hAdapter = dev->adapter; D3DKMTCloseAdapter(&c); }
    dev->adapter = 0;
}

int bc250_escape(bc250_device* dev, BC250_ESCAPE* e)
{
    int r = bc250__raw(dev->adapter, e);
    return r != BC250_OK ? r : e->Header.Status;
}

int bc250_get_info(bc250_device* dev, BC250_INFO* out)
{
    BC250_ESCAPE* e = (BC250_ESCAPE*)calloc(1, sizeof(BC250_ESCAPE));
    if (!e) return BC250_E_NOMEM;
    e->Header.Command = BC250_CMD_GET_INFO;
    int r = bc250_escape(dev, e);
    if (r == BC250_OK) *out = e->u.Info;
    free(e);
    return r;
}

int bc250_submit_ib(bc250_device* dev, const bc_u32* ib, bc_u32 count, bc_u32* fence, bc_u32* us, bc_u64* scratch_mc)
{
    if (count > BC250_IB_MAX_DWORDS) return BC250_E_RANGE;
    BC250_ESCAPE* e = (BC250_ESCAPE*)calloc(1, sizeof(BC250_ESCAPE));
    if (!e) return BC250_E_NOMEM;
    e->Header.Command = BC250_CMD_SUBMIT_IB;
    e->u.Submit.Count = count;
    memcpy(e->u.Submit.Ib, ib, count * sizeof(bc_u32));
    int r = bc250_escape(dev, e);
    if (fence) *fence = e->u.Submit.Fence;
    if (us) *us = e->u.Submit.ElapsedUs;
    if (scratch_mc) *scratch_mc = e->u.Submit.ScratchMc;
    free(e);
    return r;
}

int bc250_fill_rect(bc250_device* dev, bc_u32 x, bc_u32 y, bc_u32 w, bc_u32 h, bc_u32 xrgb)
{
    BC250_ESCAPE* e = (BC250_ESCAPE*)calloc(1, sizeof(BC250_ESCAPE));
    if (!e) return BC250_E_NOMEM;
    e->Header.Command = BC250_CMD_FILL_RECT;
    e->u.Rect.X = x; e->u.Rect.Y = y; e->u.Rect.Width = w; e->u.Rect.Height = h; e->u.Rect.Color = xrgb;
    int r = bc250_escape(dev, e);
    free(e);
    return r;
}

int bc250_vram_read(bc250_device* dev, bc_u32 offset, bc_u32 count, bc_u32* out)
{
    if (count > 64) return BC250_E_RANGE;
    BC250_ESCAPE* e = (BC250_ESCAPE*)calloc(1, sizeof(BC250_ESCAPE));
    if (!e) return BC250_E_NOMEM;
    e->Header.Command = BC250_CMD_VRAM_READ;
    e->u.Vram.Offset = offset; e->u.Vram.Count = count;
    int r = bc250_escape(dev, e);
    if (r == BC250_OK) memcpy(out, e->u.Vram.Data, count * sizeof(bc_u32));
    free(e);
    return r;
}

const char* bc250_strerror(int s)
{
    switch (s)
    {
    case BC250_OK: return "OK";
    case BC250_E_BADCMD: return "unknown command";
    case BC250_E_DENIED: return "denied (DeveloperMode is off)";
    case BC250_E_NOHW: return "driver not reachable / register BAR not mapped";
    case BC250_E_RANGE: return "out of range / bad parameter";
    case BC250_E_TIMEOUT: return "timeout";
    case BC250_E_STATE: return "prerequisite missing";
    case BC250_E_NOMEM: return "out of memory / VRAM window unavailable";
    case BC250_E_SMU: return "SMU rejected the message";
    case BC250_E_VERIFY: return "verification failed";
    }
    return "?";
}
#endif // BC250_IMPLEMENTATION
