// SPDX-License-Identifier: MIT
// BC250Kmd.sys - full WDDM 1.2 kernel-mode driver for the AMD BC-250 (milestone M5b).
//
// Display side: reuses the KMDOD class (VidPn, monitors, pointer, POST framebuffer ownership).
// Render side (this file):
//   * two segments: CPU-visible VRAM (carve-out through BAR0) and a GART aperture segment
//   * allocations described by BC250_KMD_ALLOC_DESC (UMD) or standard allocations (dxgkrnl)
//   * paging (transfer/fill) built as SDMA packets, aperture map/unmap through the GART
//   * DMA buffers submitted to SDMA0 as indirect buffers, completion via FENCE + TRAP + IH
//   * Present blt/color-fill built as SDMA COPY/FILL; blts to the primary go to the scanout buffer
//   * TDR: halt and restart SDMA
// Shaders are not available: graphics/compute (CP/RLC) is not brought up yet. See docs/ROADMAP.md.
//
// Built from the same sources as BC250Display.sys with BC250_KMD defined; installed with a
// separate INF (package/bc250kmd.inf) only when explicitly selected.

#include "..\bdd.hxx"
#include "bc250_kmd_shared.h"

#pragma code_seg("PAGE")

#define KMD_TAG                 'K052'
#define KMD_DMA_BUFFER_SIZE     (64 * 1024)
#define KMD_PAGING_BUFFER_SIZE  (64 * 1024)
#define KMD_ALLOCLIST_SIZE      1024
#define KMD_PATCHLIST_SIZE      1024
#define KMD_MAX_COPY            0x200000u     // bytes per SDMA COPY/FILL packet

struct KMD_ALLOCATION
{
    BC250_KMD_ALLOC_DESC Desc;
};

struct KMD_DEVICE
{
    BASIC_DISPLAY_DRIVER* pAdapter;
    HANDLE                hRuntime;
};

struct KMD_CONTEXT
{
    KMD_DEVICE* pDevice;
    UINT        NodeOrdinal;
};

static BASIC_DISPLAY_DRIVER* Adapter(HANDLE h) { return reinterpret_cast<BASIC_DISPLAY_DRIVER*>(h); }

// ----------------------------------------------------------------------------------------------
// Adapter lifetime
// ----------------------------------------------------------------------------------------------

NTSTATUS KmdStartDevice(_In_ VOID* pCtx, _In_ DXGK_START_INFO* pStartInfo, _In_ DXGKRNL_INTERFACE* pDxgk,
                        _Out_ ULONG* pViews, _Out_ ULONG* pChildren)
{
    PAGED_CODE();
    NTSTATUS St = BddDdiStartDevice(pCtx, pStartInfo, pDxgk, pViews, pChildren);
    if (!NT_SUCCESS(St)) return St;
    St = Adapter(pCtx)->Gpu()->KmdBringUp();
    if (!NT_SUCCESS(St))
    {
        BDD_LOG_ERROR1("BC250Kmd: GPU bring-up failed 0x%I64x", St);
        BddDdiStopDevice(pCtx);
    }
    return St;
}

// ----------------------------------------------------------------------------------------------
// Adapter info: caps, segments, UMD private data
// ----------------------------------------------------------------------------------------------

NTSTATUS APIENTRY KmdQueryAdapterInfo(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_QUERYADAPTERINFO* p)
{
    PAGED_CODE();
    BC250_GPU* Gpu = Adapter(hAdapter)->Gpu();

    switch (p->Type)
    {
    case DXGKQAITYPE_DRIVERCAPS:
    {
        if (p->OutputDataSize < sizeof(DXGK_DRIVERCAPS)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_DRIVERCAPS* c = (DXGK_DRIVERCAPS*)p->pOutputData;
        RtlZeroMemory(c, sizeof(*c));
        c->HighestAcceptableAddress.QuadPart = 0x0000FFFFFFFFFFFFll;   // 48-bit GART PTEs
        c->MaxAllocationListSlotId = KMD_ALLOCLIST_SIZE;
        c->ApertureSegmentCommitLimit = (SIZE_T)Gpu->ApertureSize();
        c->PresentationCaps.SupportKernelModeCommandBuffer = FALSE;
        c->SchedulingCaps.MultiEngineAware = 0;
        c->MemoryManagementCaps.PagingNode = 0;
        c->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        c->WDDMVersion = DXGKDDI_WDDMv1_2;
        c->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        c->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        c->SupportNonVGA = TRUE;
        c->SupportSmoothRotation = FALSE;
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_QUERYSEGMENT3:
    {
        if (p->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT3)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_QUERYSEGMENTOUT3* o = (DXGK_QUERYSEGMENTOUT3*)p->pOutputData;
        if (!o->pSegmentDescriptor)
        {
            o->NbSegment = 2;
            return STATUS_SUCCESS;
        }
        if (o->NbSegment < 2) return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(o->pSegmentDescriptor, 2 * sizeof(DXGK_SEGMENTDESCRIPTOR3));

        DXGK_SEGMENTDESCRIPTOR3* v = &o->pSegmentDescriptor[0];       // segment 1: VRAM
        v->BaseAddress.QuadPart = (LONGLONG)Gpu->VramSegmentMc();
        v->CpuTranslatedAddress.QuadPart = (LONGLONG)Gpu->VramSegmentCpu();
        v->Size = (SIZE_T)Gpu->VramSegmentSize();
        v->CommitLimit = v->Size;
        v->Flags.CpuVisible = 1;

        DXGK_SEGMENTDESCRIPTOR3* a = &o->pSegmentDescriptor[1];       // segment 2: GART aperture
        a->BaseAddress.QuadPart = (LONGLONG)Gpu->ApertureBase();
        a->Size = (SIZE_T)Gpu->ApertureSize();
        a->CommitLimit = a->Size;
        a->Flags.Aperture = 1;
        a->Flags.CacheCoherent = 1;                                    // snooped PTEs

        o->NbSegment = 2;
        o->PagingBufferSegmentId = BC250_KMD_SEGMENT_APERTURE;
        o->PagingBufferSize = KMD_PAGING_BUFFER_SIZE;
        o->PagingBufferPrivateDataSize = 0;
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_UMDRIVERPRIVATE:
    {
        if (p->OutputDataSize < sizeof(BC250_KMD_ADAPTER_INFO)) return STATUS_BUFFER_TOO_SMALL;
        BC250_KMD_ADAPTER_INFO* i = (BC250_KMD_ADAPTER_INFO*)p->pOutputData;
        RtlZeroMemory(i, sizeof(*i));
        i->Magic = BC250_KMD_ALLOC_MAGIC;
        i->Version = 1;
        i->VramSegmentMc = Gpu->VramSegmentMc();
        i->VramSegmentSize = Gpu->VramSegmentSize();
        i->ApertureBase = Gpu->ApertureBase();
        i->ApertureSize = Gpu->ApertureSize();
        i->ScanoutMc = Gpu->ScanoutMc();
        i->ScanoutPitch = Gpu->ScanoutPitch();
        i->Engines = 1;
        return STATUS_SUCCESS;
    }

    default:
        return BddDdiQueryAdapterInfo(hAdapter, p);   // display caps extension etc.
    }
}

// ----------------------------------------------------------------------------------------------
// Devices, contexts
// ----------------------------------------------------------------------------------------------

NTSTATUS APIENTRY KmdCreateDevice(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEDEVICE* p)
{
    PAGED_CODE();
    KMD_DEVICE* d = (KMD_DEVICE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KMD_DEVICE), KMD_TAG);
    if (!d) return STATUS_NO_MEMORY;
    d->pAdapter = Adapter(hAdapter);
    d->hRuntime = p->hDevice;
    p->hDevice = d;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdDestroyDevice(_In_ CONST HANDLE hDevice)
{
    PAGED_CODE();
    ExFreePoolWithTag(hDevice, KMD_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdCreateContext(_In_ CONST HANDLE hDevice, _Inout_ DXGKARG_CREATECONTEXT* p)
{
    PAGED_CODE();
    KMD_CONTEXT* c = (KMD_CONTEXT*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KMD_CONTEXT), KMD_TAG);
    if (!c) return STATUS_NO_MEMORY;
    c->pDevice = (KMD_DEVICE*)hDevice;
    c->NodeOrdinal = p->NodeOrdinal;
    p->hContext = c;
    RtlZeroMemory(&p->ContextInfo, sizeof(p->ContextInfo));
    p->ContextInfo.DmaBufferSize = KMD_DMA_BUFFER_SIZE;
    p->ContextInfo.DmaBufferSegmentSet = 1u << (BC250_KMD_SEGMENT_APERTURE - 1);   // DMA buffers live in the GART
    p->ContextInfo.DmaBufferPrivateDataSize = 0;
    p->ContextInfo.AllocationListSize = KMD_ALLOCLIST_SIZE;
    p->ContextInfo.PatchLocationListSize = KMD_PATCHLIST_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdDestroyContext(_In_ CONST HANDLE hContext)
{
    PAGED_CODE();
    ExFreePoolWithTag(hContext, KMD_TAG);
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------------------------
// Allocations
// ----------------------------------------------------------------------------------------------

static void DescribeSurface(BC250_KMD_ALLOC_DESC* d, UINT Width, UINT Height, D3DDDIFORMAT Format, UINT Flags)
{
    RtlZeroMemory(d, sizeof(*d));
    d->Magic = BC250_KMD_ALLOC_MAGIC;
    d->Flags = Flags;
    d->Width = Width;
    d->Height = Height;
    d->Format = (bckmd_u32)Format;
    d->Pitch = (Width * 4 + 255) & ~255u;   // 32-bpp formats only
    d->Size = ((bckmd_u64)d->Pitch * Height + 0xFFFF) & ~0xFFFFull;
}

NTSTATUS APIENTRY KmdCreateAllocation(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEALLOCATION* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    for (UINT i = 0; i < p->NumAllocations; ++i)
    {
        DXGK_ALLOCATIONINFO* ai = &p->pAllocationInfo[i];
        if (!ai->pPrivateDriverData || ai->PrivateDriverDataSize < sizeof(BC250_KMD_ALLOC_DESC)) return STATUS_INVALID_PARAMETER;
        const BC250_KMD_ALLOC_DESC* in = (const BC250_KMD_ALLOC_DESC*)ai->pPrivateDriverData;
        if (in->Magic != BC250_KMD_ALLOC_MAGIC || !in->Size || in->Size > 0x40000000ull) return STATUS_INVALID_PARAMETER;

        KMD_ALLOCATION* a = (KMD_ALLOCATION*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KMD_ALLOCATION), KMD_TAG);
        if (!a) return STATUS_NO_MEMORY;
        a->Desc = *in;

        ai->hAllocation = a;
        ai->Size = (SIZE_T)((in->Size + 0xFFF) & ~0xFFFull);
        ai->Alignment = 0x1000;
        ai->PitchAlignedSize = 0;
        ai->HintedBank.Value = 0;
        ai->PreferredSegment.Value = 0;
        ai->Flags.Value = 0;
        if (in->Flags & BC250_KMD_ALLOC_STAGING)
        {
            ai->SupportedReadSegmentSet = 1u << (BC250_KMD_SEGMENT_APERTURE - 1);
            ai->SupportedWriteSegmentSet = 1u << (BC250_KMD_SEGMENT_APERTURE - 1);
            ai->Flags.CpuVisible = 1;
        }
        else
        {
            // VRAM preferred, the aperture works too (SDMA reaches both)
            ai->SupportedReadSegmentSet = 0x3;
            ai->SupportedWriteSegmentSet = 0x3;
            ai->PreferredSegment.SegmentId0 = BC250_KMD_SEGMENT_VRAM;
            ai->PreferredSegment.Direction0 = 0;
            if (in->Flags & (BC250_KMD_ALLOC_PRIMARY | BC250_KMD_ALLOC_CPUVISIBLE)) ai->Flags.CpuVisible = 1;
        }
        ai->EvictionSegmentSet = 0;   // evict to system memory
        ai->MaximumRenamingListLength = 0;
        ai->pAllocationUsageHint = NULL;
        ai->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdDestroyAllocation(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_DESTROYALLOCATION* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    for (UINT i = 0; i < p->NumAllocations; ++i)
        if (p->pAllocationList[i]) ExFreePoolWithTag(p->pAllocationList[i], KMD_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdDescribeAllocation(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_DESCRIBEALLOCATION* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    const KMD_ALLOCATION* a = (const KMD_ALLOCATION*)p->hAllocation;
    p->Width = a->Desc.Width;
    p->Height = a->Desc.Height;
    p->Format = (D3DDDIFORMAT)a->Desc.Format;
    p->MultisampleMethod.NumSamples = 0;
    p->MultisampleMethod.NumQualityLevels = 0;
    p->RefreshRate.Numerator = a->Desc.RefreshNum;
    p->RefreshRate.Denominator = a->Desc.RefreshDen;
    p->PrivateDriverFormatAttribute = 0;
    p->Flags.Value = 0;
    p->Rotation = D3DDDI_ROTATION_IDENTITY;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdGetStandardAllocationDriverData(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    p->ResourcePrivateDriverDataSize = 0;
    if (!p->pAllocationPrivateDriverData)
    {
        p->AllocationPrivateDriverDataSize = sizeof(BC250_KMD_ALLOC_DESC);
        return STATUS_SUCCESS;
    }
    BC250_KMD_ALLOC_DESC* d = (BC250_KMD_ALLOC_DESC*)p->pAllocationPrivateDriverData;
    switch (p->StandardAllocationType)
    {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
    {
        D3DKMDT_SHAREDPRIMARYSURFACEDATA* s = p->pCreateSharedPrimarySurfaceData;
        DescribeSurface(d, s->Width, s->Height, s->Format, BC250_KMD_ALLOC_PRIMARY);
        d->VidPnSourceId = s->VidPnSourceId;
        d->RefreshNum = s->RefreshRate.Numerator;
        d->RefreshDen = s->RefreshRate.Denominator;
        break;
    }
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
    {
        D3DKMDT_SHADOWSURFACEDATA* s = p->pCreateShadowSurfaceData;
        DescribeSurface(d, s->Width, s->Height, s->Format, BC250_KMD_ALLOC_SHADOW | BC250_KMD_ALLOC_CPUVISIBLE);
        s->Pitch = d->Pitch;
        break;
    }
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
    {
        D3DKMDT_STAGINGSURFACEDATA* s = p->pCreateStagingSurfaceData;
        DescribeSurface(d, s->Width, s->Height, D3DDDIFMT_X8R8G8B8, BC250_KMD_ALLOC_STAGING);
        s->Pitch = d->Pitch;
        break;
    }
    case D3DKMDT_STANDARDALLOCATION_GDISURFACE:
    {
        D3DKMDT_GDISURFACEDATA* s = p->pCreateGdiSurfaceData;
        DescribeSurface(d, s->Width, s->Height, s->Format, BC250_KMD_ALLOC_CPUVISIBLE);
        s->Pitch = d->Pitch;
        break;
    }
    default:
        return STATUS_NOT_SUPPORTED;
    }
    p->AllocationPrivateDriverDataSize = sizeof(BC250_KMD_ALLOC_DESC);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdOpenAllocation(_In_ CONST HANDLE hDevice, _In_ CONST DXGKARG_OPENALLOCATION* p)
{
    PAGED_CODE();
    KMD_DEVICE* d = (KMD_DEVICE*)hDevice;
    const DXGKRNL_INTERFACE* Dxgk = d->pAdapter->GetDxgkInterface();
    for (UINT i = 0; i < p->NumAllocations; ++i)
    {
        DXGKARGCB_GETHANDLEDATA g = {};
        g.hObject = p->pOpenAllocation[i].hAllocation;
        g.Type = DXGK_HANDLE_ALLOCATION;
        p->pOpenAllocation[i].hDeviceSpecificAllocation = Dxgk->DxgkCbGetHandleData(&g);
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdCloseAllocation(_In_ CONST HANDLE hDevice, _In_ CONST DXGKARG_CLOSEALLOCATION* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(p);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdAcquireSwizzlingRange(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_ACQUIRESWIZZLINGRANGE* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(p);
    return STATUS_SUCCESS;   // linear surfaces only
}

NTSTATUS APIENTRY KmdReleaseSwizzlingRange(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_RELEASESWIZZLINGRANGE* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(p);
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------------------------
// Command building helpers (SDMA v5 packets)
// ----------------------------------------------------------------------------------------------

#pragma code_seg(push)
#pragma code_seg()

static UINT EmitCopy(ULONG* p, ULONGLONG Src, ULONGLONG Dst, ULONG Bytes)
{
    p[0] = 1u;                       // SDMA_OP_COPY, COPY_LINEAR
    p[1] = Bytes - 1; p[2] = 0;
    p[3] = (ULONG)Src; p[4] = (ULONG)(Src >> 32);
    p[5] = (ULONG)Dst; p[6] = (ULONG)(Dst >> 32);
    return 7;
}

static UINT EmitFill(ULONG* p, ULONGLONG Dst, ULONG Value, ULONG Bytes)
{
    p[0] = 11u | (2u << 30);         // SDMA_OP_CONST_FILL, dword
    p[1] = (ULONG)Dst; p[2] = (ULONG)(Dst >> 32);
    p[3] = Value; p[4] = Bytes - 1;
    return 5;
}

// ----------------------------------------------------------------------------------------------
// Submission path (called by the scheduler; must be non-paged)
// ----------------------------------------------------------------------------------------------

NTSTATUS APIENTRY KmdPatch(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PATCH* p)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UCHAR* Buf = (UCHAR*)p->pDmaBuffer;
    for (UINT i = p->PatchLocationListSubmissionStart;
         i < p->PatchLocationListSubmissionStart + p->PatchLocationListSubmissionLength; ++i)
    {
        const D3DDDI_PATCHLOCATIONLIST* l = &p->pPatchLocationList[i];
        if (l->AllocationIndex >= p->AllocationListSize || l->PatchOffset + 8 > p->DmaBufferSize) continue;
        const DXGK_ALLOCATIONLIST* a = &p->pAllocationList[l->AllocationIndex];
        if (!a->SegmentId) continue;
        ULONGLONG Addr = (ULONGLONG)a->PhysicalAddress.QuadPart + l->AllocationOffset;
        *(ULONG*)(Buf + l->PatchOffset) = (ULONG)Addr;
        *(ULONG*)(Buf + l->PatchOffset + 4) = (ULONG)(Addr >> 32);
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdSubmitCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SUBMITCOMMAND* p)
{
    BC250_GPU* Gpu = Adapter(hAdapter)->Gpu();
    ULONGLONG Ib = (ULONGLONG)p->DmaBufferPhysicalAddress.QuadPart + p->DmaBufferSubmissionStartOffset;
    ULONG Dwords = (p->DmaBufferSubmissionEndOffset - p->DmaBufferSubmissionStartOffset) / 4;
    if (p->DmaBufferSegmentId != BC250_KMD_SEGMENT_APERTURE && p->DmaBufferSegmentId != BC250_KMD_SEGMENT_VRAM) Dwords = 0;
    Gpu->KmdSubmit(Ib, Dwords, p->SubmissionFenceId);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdPreemptCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PREEMPTCOMMAND* p)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(p);
    return STATUS_SUCCESS;   // DMA-buffer-boundary preemption: work completes, no mid-buffer preemption
}

NTSTATUS APIENTRY KmdQueryCurrentFence(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYCURRENTFENCE* p)
{
    p->CurrentFence = Adapter(hAdapter)->Gpu()->KmdCompletedFence();
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdSetVidPnSourceAddress(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETVIDPNSOURCEADDRESS* p)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(p);
    // Scanout stays on the GOP surface (DCN is not programmed yet); presents to the primary are
    // redirected to it in KmdPresent, so there is nothing to flip.
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdControlInterrupt(_In_ CONST HANDLE hAdapter, _In_ CONST DXGK_INTERRUPT_TYPE Type, _In_ BOOLEAN Enable)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(Enable);
    return STATUS_NOT_IMPLEMENTED;   // no vsync interrupt without DCN
}

NTSTATUS APIENTRY KmdGetScanLine(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_GETSCANLINE* p)
{
    UNREFERENCED_PARAMETER(hAdapter);
    p->InVerticalBlank = FALSE;
    p->ScanLine = 0;
    return STATUS_SUCCESS;
}

#pragma code_seg(pop)

// ----------------------------------------------------------------------------------------------
// Paging
// ----------------------------------------------------------------------------------------------

static NTSTATUS CpuTransferMdl(BC250_GPU* Gpu, ULONGLONG SegAddr, MDL* pMdl, UINT MdlPageOffset, SIZE_T Offset, SIZE_T Size, BOOLEAN ToMdl)
{
    // VRAM <-> system memory without a GART window: wait for SDMA, then copy with the CPU through BAR0.
    if (!Gpu->KmdWaitIdle(2000000)) return STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE;
    UCHAR* Sys = (UCHAR*)MmGetSystemAddressForMdlSafe(pMdl, NormalPagePriority | MdlMappingNoExecute);
    if (!Sys) return STATUS_INSUFFICIENT_RESOURCES;
    Sys += (SIZE_T)MdlPageOffset * PAGE_SIZE;
    PHYSICAL_ADDRESS Pa;
    Pa.QuadPart = (LONGLONG)(Gpu->VramSegmentCpu() + (SegAddr - Gpu->VramSegmentMc()) + Offset);
    PVOID Io = MmMapIoSpaceEx(Pa, Size, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!Io) return STATUS_INSUFFICIENT_RESOURCES;
    if (ToMdl) RtlCopyMemory(Sys + Offset, Io, Size);
    else       RtlCopyMemory(Io, Sys + Offset, Size);
    MmUnmapIoSpace(Io, Size);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdBuildPagingBuffer(_In_ CONST HANDLE hAdapter, _In_ DXGKARG_BUILDPAGINGBUFFER* p)
{
    PAGED_CODE();
    BC250_GPU* Gpu = Adapter(hAdapter)->Gpu();
    ULONG* Out = (ULONG*)p->pDmaBuffer;
    UINT Room = p->DmaSize / 4;
    UINT Used = 0;

    switch (p->Operation)
    {
    case DXGK_OPERATION_TRANSFER:
    {
        auto& t = p->Transfer;
        const BOOLEAN SrcSeg = t.Source.SegmentId != 0, DstSeg = t.Destination.SegmentId != 0;
        if (SrcSeg && DstSeg)
        {
            SIZE_T Done = p->MultipassOffset;
            while (Done < t.TransferSize)
            {
                if (Used + 7 > Room) { p->MultipassOffset = (UINT)Done; p->pDmaBuffer = Out + Used; return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER; }
                ULONG Chunk = (ULONG)min((SIZE_T)KMD_MAX_COPY, t.TransferSize - Done);
                Used += EmitCopy(Out + Used,
                                 (ULONGLONG)t.Source.SegmentAddress.QuadPart + t.TransferOffset + Done,
                                 (ULONGLONG)t.Destination.SegmentAddress.QuadPart + t.TransferOffset + Done, Chunk);
                Done += Chunk;
            }
        }
        else if (SrcSeg && t.Source.SegmentId == BC250_KMD_SEGMENT_VRAM && t.Destination.pMdl)
        {
            NTSTATUS St = CpuTransferMdl(Gpu, (ULONGLONG)t.Source.SegmentAddress.QuadPart, t.Destination.pMdl, t.MdlOffset,
                                         t.TransferOffset, t.TransferSize, TRUE);
            if (!NT_SUCCESS(St)) return St;
        }
        else if (DstSeg && t.Destination.SegmentId == BC250_KMD_SEGMENT_VRAM && t.Source.pMdl)
        {
            NTSTATUS St = CpuTransferMdl(Gpu, (ULONGLONG)t.Destination.SegmentAddress.QuadPart, t.Source.pMdl, t.MdlOffset,
                                         t.TransferOffset, t.TransferSize, FALSE);
            if (!NT_SUCCESS(St)) return St;
        }
        // aperture <-> system memory: same physical pages, nothing to move
        break;
    }

    case DXGK_OPERATION_FILL:
    {
        auto& f = p->Fill;
        SIZE_T Done = p->MultipassOffset;
        while (Done < f.FillSize)
        {
            if (Used + 5 > Room) { p->MultipassOffset = (UINT)Done; p->pDmaBuffer = Out + Used; return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER; }
            ULONG Chunk = (ULONG)min((SIZE_T)KMD_MAX_COPY, f.FillSize - Done) & ~3u;
            if (!Chunk) break;
            Used += EmitFill(Out + Used, (ULONGLONG)f.Destination.SegmentAddress.QuadPart + Done, f.FillPattern, Chunk);
            Done += Chunk;
        }
        break;
    }

    case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
    {
        auto& m = p->MapApertureSegment;
        if (m.SegmentId != BC250_KMD_SEGMENT_APERTURE || !m.pMdl) return STATUS_INVALID_PARAMETER;
        PFN_NUMBER* Pfns = MmGetMdlPfnArray(m.pMdl) + m.MdlOffset;
        Gpu->GartMapPages(Gpu->ApertureBase() + (ULONGLONG)m.OffsetInPages * PAGE_SIZE, Pfns, m.NumberOfPages);
        break;
    }

    case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT:
    {
        auto& u = p->UnmapApertureSegment;
        if (u.SegmentId != BC250_KMD_SEGMENT_APERTURE) return STATUS_INVALID_PARAMETER;
        Gpu->GartUnmapPages(Gpu->ApertureBase() + (ULONGLONG)u.OffsetInPages * PAGE_SIZE, u.NumberOfPages);
        break;
    }

    default:
        break;   // discard content, special lock transfers, etc.: nothing to do
    }
    p->pDmaBuffer = Out + Used;
    p->MultipassOffset = 0;
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------------------------
// Render / Present
// ----------------------------------------------------------------------------------------------

NTSTATUS APIENTRY KmdRender(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_RENDER* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hContext);
    // The UMD builds SDMA packets directly; validate sizes and pass them through with their patch list.
    if (p->CommandLength > p->DmaSize) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    if (p->PatchLocationListInSize > p->PatchLocationListOutSize) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    RtlCopyMemory(p->pDmaBuffer, p->pCommand, p->CommandLength);
    RtlCopyMemory(p->pPatchLocationListOut, p->pPatchLocationListIn, p->PatchLocationListInSize * sizeof(D3DDDI_PATCHLOCATIONLIST));
    p->pDmaBuffer = (UCHAR*)p->pDmaBuffer + p->CommandLength;
    p->pPatchLocationListOut += p->PatchLocationListInSize;
    return STATUS_SUCCESS;
}

static BOOLEAN AddPatch(DXGKARG_PRESENT* p, UINT AllocIndex, UINT AllocOffset, UINT PatchOffset)
{
    if (!p->PatchLocationListOutSize) return FALSE;
    D3DDDI_PATCHLOCATIONLIST* l = p->pPatchLocationListOut;
    RtlZeroMemory(l, sizeof(*l));
    l->AllocationIndex = AllocIndex;
    l->AllocationOffset = AllocOffset;
    l->PatchOffset = PatchOffset;
    ++p->pPatchLocationListOut;
    --p->PatchLocationListOutSize;
    return TRUE;
}

NTSTATUS APIENTRY KmdPresent(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_PRESENT* p)
{
    PAGED_CODE();
    KMD_CONTEXT* c = (KMD_CONTEXT*)hContext;
    BC250_GPU* Gpu = c->pDevice->pAdapter->Gpu();
    const DXGK_ALLOCATIONLIST* Src = &p->pAllocationList[DXGK_PRESENT_SOURCE_INDEX];
    const DXGK_ALLOCATIONLIST* Dst = &p->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX];
    const KMD_ALLOCATION* SrcA = (const KMD_ALLOCATION*)Src->hDeviceSpecificAllocation;
    const KMD_ALLOCATION* DstA = (const KMD_ALLOCATION*)Dst->hDeviceSpecificAllocation;
    if (!DstA) return STATUS_INVALID_PARAMETER;

    // Blts to the primary go straight to the scanout surface (no DCN flips yet)
    const BOOLEAN ToScanout = (DstA->Desc.Flags & BC250_KMD_ALLOC_PRIMARY) != 0 && Gpu->ScanoutMc() != 0;
    const UINT DstPitch = ToScanout ? Gpu->ScanoutPitch() : DstA->Desc.Pitch;

    ULONG* Out = (ULONG*)p->pDmaBuffer;
    const UINT Room = p->DmaSize / 4;
    UINT Used = 0;
    UINT Rects = p->SubRectCnt ? p->SubRectCnt : 1;
    UINT RectIdx = p->MultipassOffset >> 16, Row = p->MultipassOffset & 0xFFFF;

    for (; RectIdx < Rects; ++RectIdx, Row = 0)
    {
        RECT d = p->SubRectCnt ? p->pDstSubRects[RectIdx] : p->DstRect;
        LONG dx = d.left - p->DstRect.left, dy = d.top - p->DstRect.top;
        RECT s = { p->SrcRect.left + dx, p->SrcRect.top + dy, 0, 0 };
        if (d.right <= d.left || d.bottom <= d.top) continue;
        ULONG Bytes = (ULONG)(d.right - d.left) * 4;

        for (; Row < (UINT)(d.bottom - d.top); ++Row)
        {
            if (Used + 7 > Room || p->PatchLocationListOutSize < 2)
            {
                p->MultipassOffset = (RectIdx << 16) | Row;
                p->pDmaBuffer = Out + Used;
                return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
            }
            UINT DstOff = (UINT)(d.top + Row) * DstPitch + (UINT)d.left * 4;
            UINT Base = Used;
            if (p->Flags.ColorFill)
            {
                ULONGLONG DstAddr = ToScanout ? Gpu->ScanoutMc() + DstOff : 0;
                Used += EmitFill(Out + Used, DstAddr, p->Color, Bytes);
                if (!ToScanout) AddPatch(p, DXGK_PRESENT_DESTINATION_INDEX, DstOff, (Base + 1) * 4);
            }
            else
            {
                if (!SrcA) return STATUS_INVALID_PARAMETER;
                UINT SrcOff = (UINT)(s.top + Row) * SrcA->Desc.Pitch + (UINT)s.left * 4;
                ULONGLONG DstAddr = ToScanout ? Gpu->ScanoutMc() + DstOff : 0;
                Used += EmitCopy(Out + Used, 0, DstAddr, Bytes);
                AddPatch(p, DXGK_PRESENT_SOURCE_INDEX, SrcOff, (Base + 3) * 4);
                if (!ToScanout) AddPatch(p, DXGK_PRESENT_DESTINATION_INDEX, DstOff, (Base + 5) * 4);
            }
        }
    }
    p->pDmaBuffer = Out + Used;
    p->MultipassOffset = 0;
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------------------------
// TDR, misc
// ----------------------------------------------------------------------------------------------

NTSTATUS APIENTRY KmdResetFromTimeout(_In_ CONST HANDLE hAdapter)
{
    PAGED_CODE();
    Adapter(hAdapter)->Gpu()->KmdHalt();
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdRestartFromTimeout(_In_ CONST HANDLE hAdapter)
{
    PAGED_CODE();
    return Adapter(hAdapter)->Gpu()->KmdRestart();
}

NTSTATUS APIENTRY KmdCollectDbgInfo(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_COLLECTDBGINFO* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    if (p->pBuffer && p->BufferSize) RtlZeroMemory(p->pBuffer, p->BufferSize);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdSetPalette(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETPALETTE* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(p);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdRecommendVidPnTopology(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(p);
    return STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY;
}

NTSTATUS APIENTRY KmdStopCapture(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_STOPCAPTURE* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(p);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY KmdCreateOverlay(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEOVERLAY* p)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(p);
    return STATUS_NOT_SUPPORTED;
}

// ----------------------------------------------------------------------------------------------
// DriverEntry
// ----------------------------------------------------------------------------------------------

extern "C" DRIVER_INITIALIZE DriverEntry;

#pragma code_seg("INIT")
extern "C" NTSTATUS DriverEntry(_In_ DRIVER_OBJECT* pDriverObject, _In_ UNICODE_STRING* pRegistryPath)
{
    DRIVER_INITIALIZATION_DATA d;
    RtlZeroMemory(&d, sizeof(d));
    d.Version = DXGKDDI_INTERFACE_VERSION_WIN8;

    // display + lifetime: KMDOD implementation
    d.DxgkDdiAddDevice                    = BddDdiAddDevice;
    d.DxgkDdiStartDevice                  = KmdStartDevice;
    d.DxgkDdiStopDevice                   = BddDdiStopDevice;
    d.DxgkDdiRemoveDevice                 = BddDdiRemoveDevice;
    d.DxgkDdiDispatchIoRequest            = BddDdiDispatchIoRequest;
    d.DxgkDdiInterruptRoutine             = BddDdiInterruptRoutine;
    d.DxgkDdiDpcRoutine                   = BddDdiDpcRoutine;
    d.DxgkDdiQueryChildRelations          = BddDdiQueryChildRelations;
    d.DxgkDdiQueryChildStatus             = BddDdiQueryChildStatus;
    d.DxgkDdiQueryDeviceDescriptor        = BddDdiQueryDeviceDescriptor;
    d.DxgkDdiSetPowerState                = BddDdiSetPowerState;
    d.DxgkDdiResetDevice                  = BddDdiResetDevice;
    d.DxgkDdiUnload                       = BddDdiUnload;
    d.DxgkDdiSetPointerPosition           = BddDdiSetPointerPosition;
    d.DxgkDdiSetPointerShape              = BddDdiSetPointerShape;
    d.DxgkDdiEscape                       = BddDdiEscape;
    d.DxgkDdiIsSupportedVidPn             = BddDdiIsSupportedVidPn;
    d.DxgkDdiRecommendFunctionalVidPn     = BddDdiRecommendFunctionalVidPn;
    d.DxgkDdiEnumVidPnCofuncModality      = BddDdiEnumVidPnCofuncModality;
    d.DxgkDdiSetVidPnSourceVisibility     = BddDdiSetVidPnSourceVisibility;
    d.DxgkDdiCommitVidPn                  = BddDdiCommitVidPn;
    d.DxgkDdiUpdateActiveVidPnPresentPath = BddDdiUpdateActiveVidPnPresentPath;
    d.DxgkDdiRecommendMonitorModes        = BddDdiRecommendMonitorModes;
    d.DxgkDdiQueryVidPnHWCapability       = BddDdiQueryVidPnHWCapability;
    d.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = BddDdiStopDeviceAndReleasePostDisplayOwnership;
    d.DxgkDdiSystemDisplayEnable          = BddDdiSystemDisplayEnable;
    d.DxgkDdiSystemDisplayWrite           = BddDdiSystemDisplayWrite;

    // render: this file
    d.DxgkDdiQueryAdapterInfo             = KmdQueryAdapterInfo;
    d.DxgkDdiCreateDevice                 = KmdCreateDevice;
    d.DxgkDdiDestroyDevice                = KmdDestroyDevice;
    d.DxgkDdiCreateContext                = KmdCreateContext;
    d.DxgkDdiDestroyContext               = KmdDestroyContext;
    d.DxgkDdiCreateAllocation             = KmdCreateAllocation;
    d.DxgkDdiDestroyAllocation            = KmdDestroyAllocation;
    d.DxgkDdiDescribeAllocation           = KmdDescribeAllocation;
    d.DxgkDdiGetStandardAllocationDriverData = KmdGetStandardAllocationDriverData;
    d.DxgkDdiOpenAllocation               = KmdOpenAllocation;
    d.DxgkDdiCloseAllocation              = KmdCloseAllocation;
    d.DxgkDdiAcquireSwizzlingRange        = KmdAcquireSwizzlingRange;
    d.DxgkDdiReleaseSwizzlingRange        = KmdReleaseSwizzlingRange;
    d.DxgkDdiPatch                        = KmdPatch;
    d.DxgkDdiSubmitCommand                = KmdSubmitCommand;
    d.DxgkDdiPreemptCommand               = KmdPreemptCommand;
    d.DxgkDdiBuildPagingBuffer            = KmdBuildPagingBuffer;
    d.DxgkDdiSetPalette                   = KmdSetPalette;
    d.DxgkDdiResetFromTimeout             = KmdResetFromTimeout;
    d.DxgkDdiRestartFromTimeout           = KmdRestartFromTimeout;
    d.DxgkDdiCollectDbgInfo               = KmdCollectDbgInfo;
    d.DxgkDdiQueryCurrentFence            = KmdQueryCurrentFence;
    d.DxgkDdiSetVidPnSourceAddress        = KmdSetVidPnSourceAddress;
    d.DxgkDdiRecommendVidPnTopology       = KmdRecommendVidPnTopology;
    d.DxgkDdiGetScanLine                  = KmdGetScanLine;
    d.DxgkDdiStopCapture                  = KmdStopCapture;
    d.DxgkDdiControlInterrupt             = KmdControlInterrupt;
    d.DxgkDdiCreateOverlay                = KmdCreateOverlay;
    d.DxgkDdiRender                       = KmdRender;
    d.DxgkDdiRenderKm                     = KmdRender;
    d.DxgkDdiPresent                      = KmdPresent;

    return DxgkInitialize(pDriverObject, pRegistryPath, &d);
}
#pragma code_seg()
