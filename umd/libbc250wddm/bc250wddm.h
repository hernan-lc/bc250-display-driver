// SPDX-License-Identifier: MIT
// libbc250wddm - user-mode winsys for BC250Kmd.sys over the public D3DKMT thunks.
//
// This is the layer a Mesa RADV port plugs into (radeon_winsys):
//   bo_create/map/unmap  -> D3DKMTCreateAllocation / D3DKMTLock / D3DKMTUnlock
//   ctx_create           -> D3DKMTCreateContext (node 0 = SDMA, node 1 = CP graphics/PM4)
//   cs_submit            -> D3DKMTRender with an allocation list and a patch list
//                           (BC250Kmd patches GPU addresses into the DMA buffer at submit time)
//   bo_wait              -> D3DKMTLock (waits for the GPU to finish with the allocation)
//
// Header-only; define BC250WDDM_IMPLEMENTATION in exactly one source file.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include "..\..\src\kmd\bc250_kmd_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BC250W_NODE_SDMA 0u
#define BC250W_NODE_GFX  1u

typedef struct bc250w_device {
    D3DKMT_HANDLE          adapter;
    D3DKMT_HANDLE          device;
    BC250_KMD_ADAPTER_INFO info;
} bc250w_device;

typedef struct bc250w_bo {
    D3DKMT_HANDLE        handle;
    bckmd_u64            size;
    BC250_KMD_ALLOC_DESC desc;
} bc250w_bo;

typedef struct bc250w_ctx {
    bc250w_device*            dev;
    D3DKMT_HANDLE             handle;
    unsigned                  node;
    void*                     cmd;            // command buffer (DMA buffer contents) owned by dxgkrnl
    UINT                      cmd_size;
    D3DDDI_ALLOCATIONLIST*    alloc_list;
    UINT                      alloc_list_size;
    D3DDDI_PATCHLOCATIONLIST* patch_list;
    UINT                      patch_list_size;
    UINT                      cmd_used, alloc_used, patch_used;   // current submission
} bc250w_ctx;

NTSTATUS bc250w_open(bc250w_device* dev);
void     bc250w_close(bc250w_device* dev);
NTSTATUS bc250w_bo_create(bc250w_device* dev, bckmd_u64 size, bckmd_u32 flags, bc250w_bo* bo);
void     bc250w_bo_destroy(bc250w_device* dev, bc250w_bo* bo);
NTSTATUS bc250w_bo_map(bc250w_device* dev, bc250w_bo* bo, void** ptr);   // waits for GPU idle on the bo
void     bc250w_bo_unmap(bc250w_device* dev, bc250w_bo* bo);
NTSTATUS bc250w_ctx_create(bc250w_device* dev, unsigned node, bc250w_ctx* ctx);
void     bc250w_ctx_destroy(bc250w_ctx* ctx);

// Command building: emit dwords, and reference a bo address with bc250w_emit_addr (adds a patch).
UINT     bc250w_use_bo(bc250w_ctx* ctx, bc250w_bo* bo, BOOL write);        // index in the allocation list
BOOL     bc250w_emit(bc250w_ctx* ctx, bckmd_u32 dw);
BOOL     bc250w_emit_addr(bc250w_ctx* ctx, bc250w_bo* bo, bckmd_u32 offset, BOOL write);  // 2 dwords (lo, hi)
NTSTATUS bc250w_submit(bc250w_ctx* ctx);

#ifdef __cplusplus
}
#endif

// ------------------------------------------------------------------------------------------
#ifdef BC250WDDM_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "gdi32.lib")

NTSTATUS bc250w_open(bc250w_device* dev)
{
    memset(dev, 0, sizeof(*dev));
    D3DKMT_ENUMADAPTERS2 en;
    memset(&en, 0, sizeof(en));
    NTSTATUS st = D3DKMTEnumAdapters2(&en);
    if (st != 0) return st;
    D3DKMT_ADAPTERINFO* infos = (D3DKMT_ADAPTERINFO*)calloc(en.NumAdapters ? en.NumAdapters : 1, sizeof(*infos));
    if (!infos) return (NTSTATUS)0xC0000017L;   // STATUS_NO_MEMORY
    en.pAdapters = infos;
    st = D3DKMTEnumAdapters2(&en);
    for (ULONG i = 0; st == 0 && i < en.NumAdapters; ++i)
    {
        BC250_KMD_ADAPTER_INFO ai;
        memset(&ai, 0, sizeof(ai));
        D3DKMT_QUERYADAPTERINFO q;
        memset(&q, 0, sizeof(q));
        q.hAdapter = infos[i].hAdapter;
        q.Type = KMTQAITYPE_UMDRIVERPRIVATE;
        q.pPrivateDriverData = &ai;
        q.PrivateDriverDataSize = sizeof(ai);
        if (!dev->adapter && D3DKMTQueryAdapterInfo(&q) == 0 && ai.Magic == BC250_KMD_ALLOC_MAGIC)
        {
            dev->adapter = infos[i].hAdapter;
            dev->info = ai;
        }
        else
        {
            D3DKMT_CLOSEADAPTER c; c.hAdapter = infos[i].hAdapter; D3DKMTCloseAdapter(&c);
        }
    }
    free(infos);
    if (!dev->adapter) return (NTSTATUS)0xC000000EL;   // STATUS_NO_SUCH_DEVICE: BC250Kmd not installed

    D3DKMT_CREATEDEVICE cd;
    memset(&cd, 0, sizeof(cd));
    cd.hAdapter = dev->adapter;
    st = D3DKMTCreateDevice(&cd);
    if (st != 0) return st;
    dev->device = cd.hDevice;
    return 0;
}

void bc250w_close(bc250w_device* dev)
{
    if (dev->device) { D3DKMT_DESTROYDEVICE d; d.hDevice = dev->device; D3DKMTDestroyDevice(&d); }
    if (dev->adapter) { D3DKMT_CLOSEADAPTER c; c.hAdapter = dev->adapter; D3DKMTCloseAdapter(&c); }
    memset(dev, 0, sizeof(*dev));
}

NTSTATUS bc250w_bo_create(bc250w_device* dev, bckmd_u64 size, bckmd_u32 flags, bc250w_bo* bo)
{
    memset(bo, 0, sizeof(*bo));
    bo->desc.Magic = BC250_KMD_ALLOC_MAGIC;
    bo->desc.Flags = flags | BC250_KMD_ALLOC_CPUVISIBLE;
    bo->desc.Size = (size + 0xFFF) & ~0xFFFull;
    bo->desc.Width = (bckmd_u32)(bo->desc.Size / 4);
    bo->desc.Height = 1;
    bo->desc.Pitch = (bckmd_u32)bo->desc.Size;
    bo->size = bo->desc.Size;

    D3DDDI_ALLOCATIONINFO ai;
    memset(&ai, 0, sizeof(ai));
    ai.pPrivateDriverData = &bo->desc;
    ai.PrivateDriverDataSize = sizeof(bo->desc);
    D3DKMT_CREATEALLOCATION ca;
    memset(&ca, 0, sizeof(ca));
    ca.hDevice = dev->device;
    ca.NumAllocations = 1;
    ca.pAllocationInfo = &ai;
    NTSTATUS st = D3DKMTCreateAllocation(&ca);
    if (st == 0) bo->handle = ai.hAllocation;
    return st;
}

void bc250w_bo_destroy(bc250w_device* dev, bc250w_bo* bo)
{
    if (!bo->handle) return;
    D3DKMT_DESTROYALLOCATION d;
    memset(&d, 0, sizeof(d));
    d.hDevice = dev->device;
    d.phAllocationList = &bo->handle;
    d.AllocationCount = 1;
    D3DKMTDestroyAllocation(&d);
    bo->handle = 0;
}

NTSTATUS bc250w_bo_map(bc250w_device* dev, bc250w_bo* bo, void** ptr)
{
    D3DKMT_LOCK l;
    memset(&l, 0, sizeof(l));
    l.hDevice = dev->device;
    l.hAllocation = bo->handle;
    NTSTATUS st = D3DKMTLock(&l);     // no DonotWait flag: returns once the GPU is done with the bo
    *ptr = (st == 0) ? l.pData : NULL;
    return st;
}

void bc250w_bo_unmap(bc250w_device* dev, bc250w_bo* bo)
{
    D3DKMT_UNLOCK u;
    memset(&u, 0, sizeof(u));
    u.hDevice = dev->device;
    u.NumAllocations = 1;
    u.phAllocations = &bo->handle;
    D3DKMTUnlock(&u);
}

NTSTATUS bc250w_ctx_create(bc250w_device* dev, unsigned node, bc250w_ctx* ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    D3DKMT_CREATECONTEXT cc;
    memset(&cc, 0, sizeof(cc));
    cc.hDevice = dev->device;
    cc.NodeOrdinal = node;
    cc.EngineAffinity = 1;
    NTSTATUS st = D3DKMTCreateContext(&cc);
    if (st != 0) return st;
    ctx->dev = dev;
    ctx->node = node;
    ctx->handle = cc.hContext;
    ctx->cmd = cc.pCommandBuffer;
    ctx->cmd_size = cc.CommandBufferSize;
    ctx->alloc_list = cc.pAllocationList;
    ctx->alloc_list_size = cc.AllocationListSize;
    ctx->patch_list = cc.pPatchLocationList;
    ctx->patch_list_size = cc.PatchLocationListSize;
    return 0;
}

void bc250w_ctx_destroy(bc250w_ctx* ctx)
{
    if (!ctx->handle) return;
    D3DKMT_DESTROYCONTEXT d;
    d.hContext = ctx->handle;
    D3DKMTDestroyContext(&d);
    ctx->handle = 0;
}

UINT bc250w_use_bo(bc250w_ctx* ctx, bc250w_bo* bo, BOOL write)
{
    for (UINT i = 0; i < ctx->alloc_used; ++i)
        if (ctx->alloc_list[i].hAllocation == bo->handle)
        {
            if (write) ctx->alloc_list[i].WriteOperation = 1;
            return i;
        }
    if (ctx->alloc_used >= ctx->alloc_list_size) return (UINT)-1;
    D3DDDI_ALLOCATIONLIST* a = &ctx->alloc_list[ctx->alloc_used];
    memset(a, 0, sizeof(*a));
    a->hAllocation = bo->handle;
    a->WriteOperation = write ? 1 : 0;
    return ctx->alloc_used++;
}

BOOL bc250w_emit(bc250w_ctx* ctx, bckmd_u32 dw)
{
    if (ctx->cmd_used + 4 > ctx->cmd_size) return FALSE;
    memcpy((char*)ctx->cmd + ctx->cmd_used, &dw, 4);
    ctx->cmd_used += 4;
    return TRUE;
}

BOOL bc250w_emit_addr(bc250w_ctx* ctx, bc250w_bo* bo, bckmd_u32 offset, BOOL write)
{
    UINT idx = bc250w_use_bo(ctx, bo, write);
    if (idx == (UINT)-1 || ctx->patch_used >= ctx->patch_list_size) return FALSE;
    D3DDDI_PATCHLOCATIONLIST* p = &ctx->patch_list[ctx->patch_used++];
    memset(p, 0, sizeof(*p));
    p->AllocationIndex = idx;
    p->AllocationOffset = offset;
    p->PatchOffset = ctx->cmd_used;          // BC250Kmd writes the 64-bit address here (lo, hi)
    return bc250w_emit(ctx, 0) && bc250w_emit(ctx, 0);
}

NTSTATUS bc250w_submit(bc250w_ctx* ctx)
{
    D3DKMT_RENDER r;
    memset(&r, 0, sizeof(r));
    r.hContext = ctx->handle;
    r.CommandOffset = 0;
    r.CommandLength = ctx->cmd_used;
    r.AllocationCount = ctx->alloc_used;
    r.PatchLocationCount = ctx->patch_used;
    NTSTATUS st = D3DKMTRender(&r);
    if (st == 0)
    {
        ctx->cmd = r.pNewCommandBuffer;
        ctx->cmd_size = r.NewCommandBufferSize;
        ctx->alloc_list = r.pNewAllocationList;
        ctx->alloc_list_size = r.NewAllocationListSize;
        ctx->patch_list = r.pNewPatchLocationList;
        ctx->patch_list_size = r.NewPatchLocationListSize;
    }
    ctx->cmd_used = ctx->alloc_used = ctx->patch_used = 0;
    return st;
}
#endif // BC250WDDM_IMPLEMENTATION
