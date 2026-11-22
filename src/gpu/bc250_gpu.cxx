// SPDX-License-Identifier: MIT
// BC-250 hardware access layer. See bc250_gpu.h for the milestone map.

#include "..\bdd.hxx"

#pragma code_seg("PAGE")   // everything is PASSIVE_LEVEL except Isr/Rreg/Wreg (see below)

#define BC250_TAG 'G052'

// SMU (PPSMC, smu_v11_8_ppsmc.h)
#define PPSMC_Result_OK              0x1
#define PPSMC_MSG_TestMessage        0x1
#define PPSMC_MSG_GetSmuVersion      0x2
#define PPSMC_MSG_GetDriverIfVersion 0x3
#define PPSMC_MSG_QueryGfxclk        0xF
#define PPSMC_MSG_QueryVddcrSocClock 0x11
#define PPSMC_MSG_QueryDfPstate      0x13
#define PPSMC_MSG_QueryActiveWgp     0x1E
#define PPSMC_MSG_GetGfxFrequency    0x37
#define PPSMC_MSG_GetGfxVid          0x38

// PTE / PDE bits (amdgpu_vm.h)
#define PTE_VALID      (1ull << 0)
#define PTE_SYSTEM     (1ull << 1)
#define PTE_SNOOPED    (1ull << 2)
#define PTE_EXECUTABLE (1ull << 4)
#define PTE_READABLE   (1ull << 5)
#define PTE_WRITEABLE  (1ull << 6)
#define PTE_MTYPE_NV10(m) ((ULONGLONG)(m) << 48)
#define PDE_MTYPE_VG10(m) ((ULONGLONG)(m) << 57)
#define MTYPE_UC       3ull

// SDMA packets (navi10_sdma_pkt_open.h)
#define SDMA_OP_NOP    0
#define SDMA_OP_WRITE  2
#define SDMA_OP_FENCE  5
#define SDMA_OP_TRAP   6
#define SDMA_SUBOP_WRITE_LINEAR 0
#define SDMA_HDR(op, sub) (((op) & 0xFF) | (((sub) & 0xFF) << 8))

static ULONG FieldShift(ULONG Mask) { ULONG s = 0; while (Mask && !(Mask & 1)) { Mask >>= 1; ++s; } return s; }
#define SETF(v, mask, val) (((v) & ~(mask)) | ((((ULONG)(val)) << FieldShift(mask)) & (mask)))

static ULONG Log2(ULONG v) { ULONG r = 0; while (v > 1) { v >>= 1; ++r; } return r; }

BC250_GPU::BC250_GPU()
{
    RtlZeroMemory(this, sizeof(*this));
    ExInitializeFastMutex(&m_Lock);
}

// ----------------------------------------------------------------------------------------------
// M1: MMIO
// ----------------------------------------------------------------------------------------------

#pragma code_seg(push)
#pragma code_seg()   // non-paged: used from the ISR
ULONG BC250_GPU::Rreg(ULONG Dw) const
{
    if (!m_Mmio || (ULONGLONG)Dw * 4 + 4 > m_BarMmio.Length) return 0xFFFFFFFF;
    return READ_REGISTER_ULONG((PULONG)(m_Mmio + (SIZE_T)Dw * 4));
}

VOID BC250_GPU::Wreg(ULONG Dw, ULONG Value)
{
    if (!m_Mmio || (ULONGLONG)Dw * 4 + 4 > m_BarMmio.Length) return;
    WRITE_REGISTER_ULONG((PULONG)(m_Mmio + (SIZE_T)Dw * 4), Value);
}

BOOLEAN BC250_GPU::Isr()
{
    if (!m_IhReady || !m_Scratch) return FALSE;

    ULONG Wptr = Rreg(BC250_IH_RB_WPTR);
    if (Wptr == 0xFFFFFFFF) return FALSE;                 // device gone
    if (Wptr & BC250_IH_RB_WPTR__RB_OVERFLOW_MASK) InterlockedIncrement(&m_IhOverflow);
    Wptr &= BC250_IH_RB_WPTR__OFFSET_MASK;
    Wptr &= (BC250_IH_RING_SIZE - 1);
    if (Wptr == m_IhRptr) return FALSE;                   // not ours (shared line)
    BOOLEAN SdmaDone = FALSE, CpDone = FALSE;

    while (m_IhRptr != Wptr)
    {
        // navi10 IH entries are 32 bytes (8 dwords)
        ULONG Dw0 = ScrRead(BC250_SCR_IH_RING + m_IhRptr);
        ULONG Dw4 = ScrRead(BC250_SCR_IH_RING + m_IhRptr + 16);
        ULONG Client = Dw0 & 0xFF;
        ULONG Src = (Dw0 >> 8) & 0xFF;
        InterlockedExchange(&m_LastClient, (LONG)Client);
        InterlockedExchange(&m_LastSrc, (LONG)Src);
        InterlockedExchange(&m_LastSrcData, (LONG)Dw4);
        if (Client == BC250_IH_CLIENT_SDMA0 && Src == BC250_IH_SRC_SDMA_TRAP)
        {
            InterlockedIncrement64(&m_SdmaTraps);
            SdmaDone = TRUE;
        }
        else if (Client == BC250_IH_CLIENT_GRBM_CP && Src == BC250_IH_SRC_CP_EOP)
            CpDone = TRUE;
        InterlockedIncrement64(&m_IhEntries);
        m_IhRptr = (m_IhRptr + 32) & (BC250_IH_RING_SIZE - 1);
    }
    Wreg(BC250_IH_RB_RPTR, m_IhRptr);
    InterlockedIncrement64(&m_IntCount);

    if (m_KmdMode && (SdmaDone || CpDone))
    {
        // node 0: SDMA FENCE + TRAP; node 1: CP RELEASE_MEM (EOP interrupt)
        DXGKARGCB_NOTIFY_INTERRUPT_DATA d;
        if (SdmaDone)
        {
            RtlZeroMemory(&d, sizeof(d));
            d.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
            d.DmaCompleted.SubmissionFenceId = ScrRead(BC250_SCR_FENCE);
            d.DmaCompleted.NodeOrdinal = 0;
            m_Dxgk.DxgkCbNotifyInterrupt(m_Dxgk.DeviceHandle, &d);
        }
        if (CpDone)
        {
            RtlZeroMemory(&d, sizeof(d));
            d.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
            d.DmaCompleted.SubmissionFenceId = ScrRead(BC250_SCR_CP_FENCE);
            d.DmaCompleted.NodeOrdinal = 1;
            m_Dxgk.DxgkCbNotifyInterrupt(m_Dxgk.DeviceHandle, &d);
        }
        m_Dxgk.DxgkCbQueueDpc(m_Dxgk.DeviceHandle);
    }
    return TRUE;
}

VOID BC250_GPU::GartMapPages(ULONGLONG GpuAddr, const PFN_NUMBER* Pfns, SIZE_T Count)
{
    ULONGLONG First = (GpuAddr - m_GartStart) >> 12;
    for (SIZE_T i = 0; i < Count && First + i < (BC250_GART_SIZE >> 12); ++i)
    {
        ULONGLONG Pte = (((ULONGLONG)Pfns[i] << 12) & 0x0000FFFFFFFFF000ull) |
                        (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 4) | (1ull << 5) | (1ull << 6) | (3ull << 48);
        ULONG Off = BC250_SCR_GART_PT + (ULONG)((First + i) * 8);
        ScrWrite(Off, (ULONG)Pte);
        ScrWrite(Off + 4, (ULONG)(Pte >> 32));
    }
    HdpFlush();
    // TLB invalidate (VMID0)
    Wreg(BC250_GCVM_INVALIDATE_ENG17_REQ, 1u | BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PTES_MASK |
         BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE0_MASK | BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L1_PTES_MASK);
    for (ULONG i = 0; i < 10000 && !(Rreg(BC250_GCVM_INVALIDATE_ENG17_ACK) & 1u); ++i) KeStallExecutionProcessor(1);
}

VOID BC250_GPU::GartUnmapPages(ULONGLONG GpuAddr, SIZE_T Count)
{
    ULONGLONG First = (GpuAddr - m_GartStart) >> 12;
    for (SIZE_T i = 0; i < Count && First + i < (BC250_GART_SIZE >> 12); ++i)
    {
        ULONG Off = BC250_SCR_GART_PT + (ULONG)((First + i) * 8);
        ScrWrite(Off, 0);
        ScrWrite(Off + 4, 0);
    }
    HdpFlush();
    Wreg(BC250_GCVM_INVALIDATE_ENG17_REQ, 1u | BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PTES_MASK |
         BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE0_MASK | BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L1_PTES_MASK);
    for (ULONG i = 0; i < 10000 && !(Rreg(BC250_GCVM_INVALIDATE_ENG17_ACK) & 1u); ++i) KeStallExecutionProcessor(1);
}

VOID BC250_GPU::KmdSubmit(ULONGLONG IbMc, ULONG Dwords, ULONG FenceId)
{
    if (!m_SdmaReady) return;
    if (Dwords)
    {
        ULONG Nops = (2 - (m_SdmaWptr & 7)) & 7;
        for (ULONG i = 0; i < Nops; ++i) SdmaWrite(0);
        SdmaWrite(4u);                                   // SDMA_OP_INDIRECT, VMID 0
        SdmaWrite((ULONG)IbMc & 0xFFFFFFE0);
        SdmaWrite((ULONG)(IbMc >> 32));
        SdmaWrite(Dwords);
        SdmaWrite(0);
        SdmaWrite(0);
    }
    ULONGLONG F = ScrMc(BC250_SCR_FENCE);
    SdmaWrite(5u | (3u << 16));                          // SDMA_OP_FENCE, MTYPE=UC
    SdmaWrite((ULONG)F);
    SdmaWrite((ULONG)(F >> 32));
    SdmaWrite(FenceId);
    SdmaWrite(6u);                                       // SDMA_OP_TRAP
    SdmaWrite(0);
    SdmaCommit();
}
#pragma code_seg(pop)

void BC250_GPU::Start(const DXGKRNL_INTERFACE* pDxgk, const DXGK_DEVICE_INFO* pDevInfo,
                      DEVICE_OBJECT* pPdo, const DXGK_DISPLAY_INFORMATION* pScanout)
{
    PAGED_CODE();
    RtlCopyMemory(&m_Dxgk, pDxgk, sizeof(m_Dxgk));
    m_pPdo = pPdo;
    m_FbPhys   = pScanout->PhysicAddress;
    m_FbWidth  = pScanout->Width;
    m_FbHeight = pScanout->Height;
    m_FbPitch  = pScanout->Pitch;
    m_FbSize   = pScanout->Height * pScanout->Pitch;
    m_DeveloperMode = ReadDeveloperMode();

    // PCI identity
    PCI_COMMON_HEADER Hdr = {0};
    ULONG Bytes = 0;
    if (NT_SUCCESS(m_Dxgk.DxgkCbReadDeviceSpace(m_Dxgk.DeviceHandle, DXGK_WHICHSPACE_CONFIG, &Hdr, 0, sizeof(Hdr), &Bytes)))
    {
        m_VendorId = Hdr.VendorID;
        m_DeviceId = Hdr.DeviceID;
        m_Revision = Hdr.RevisionID;
        m_SubsysId = ((ULONG)Hdr.u.type0.SubSystemID << 16) | Hdr.u.type0.SubVendorID;
    }

    // BARs: largest = VRAM aperture, 512 KiB = registers, 2 MiB/remaining = doorbells
    const CM_RESOURCE_LIST* pList = pDevInfo->TranslatedResourceList;
    if (pList)
    {
        const CM_FULL_RESOURCE_DESCRIPTOR* pFull = pList->List;
        for (ULONG f = 0; f < pList->Count; ++f)
        {
            const CM_PARTIAL_RESOURCE_LIST* pPartial = &pFull->PartialResourceList;
            for (ULONG i = 0; i < pPartial->Count; ++i)
            {
                CM_PARTIAL_RESOURCE_DESCRIPTOR* d = (CM_PARTIAL_RESOURCE_DESCRIPTOR*)&pPartial->PartialDescriptors[i];
                if (d->Type == CmResourceTypeInterrupt && (d->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)) m_Msi = TRUE;
                if (d->Type != CmResourceTypeMemory && d->Type != CmResourceTypeMemoryLarge) continue;
                ULONGLONG Start = 0;
                ULONGLONG Len = RtlCmDecodeMemIoResource(d, &Start);
                if (Len == 0x80000) { m_BarMmio.PhysAddr = Start; m_BarMmio.Length = Len; }
                else if (Len > m_BarVram.Length)
                {
                    if (m_BarVram.Length && !m_BarDoorbell.Length) m_BarDoorbell = m_BarVram;
                    m_BarVram.PhysAddr = Start; m_BarVram.Length = Len;
                }
                else if (!m_BarDoorbell.Length) { m_BarDoorbell.PhysAddr = Start; m_BarDoorbell.Length = Len; }
            }
            pFull = (const CM_FULL_RESOURCE_DESCRIPTOR*)&pPartial->PartialDescriptors[pPartial->Count];
        }
    }

    if (m_BarMmio.Length)
    {
        PHYSICAL_ADDRESS Pa; Pa.QuadPart = (LONGLONG)m_BarMmio.PhysAddr;
        m_Mmio = (volatile UCHAR*)MmMapIoSpaceEx(Pa, (SIZE_T)m_BarMmio.Length, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (m_Mmio) ReadMemoryLayout();
}

void BC250_GPU::Stop()
{
    PAGED_CODE();
    ExAcquireFastMutex(&m_Lock);
    if (m_SdmaReady) SdmaHalt();
    if (m_IhReady) IhDisable();
    if (m_TestPage)
    {
        if (m_GartReady && m_Scratch)
        {
            ScrWrite(BC250_SCR_GART_PT, 0); ScrWrite(BC250_SCR_GART_PT + 4, 0);
            HdpFlush(); TlbFlush();
        }
        MmFreeContiguousMemory(m_TestPage);
        m_TestPage = NULL;
    }
    if (m_Fw) { ExFreePoolWithTag(m_Fw, BC250_TAG); m_Fw = NULL; m_FwSize = 0; }
    if (m_CpReady)
    {
        Rmw(BC250_CP_ME_CNTL, 0, BC250_CP_ME_CNTL__ME_HALT_MASK | BC250_CP_ME_CNTL__PFP_HALT_MASK | BC250_CP_ME_CNTL__CE_HALT_MASK);
        Rmw(BC250_RLC_CNTL, BC250_RLC_CNTL__RLC_ENABLE_F32_MASK, 0);
    }
    m_CpReady = FALSE;
    PspShutdown();                       // engines are halted: release TMR and KM ring
    for (ULONG i = 0; i < BC250_FW_COUNT; ++i)
        if (m_FwCp[i]) { ExFreePoolWithTag(m_FwCp[i], BC250_TAG); m_FwCp[i] = NULL; m_FwCpSize[i] = 0; }
    if (m_Scratch) { MmUnmapIoSpace((PVOID)m_Scratch, (SIZE_T)BC250_SCRATCH_SIZE); m_Scratch = NULL; }
    if (m_Mmio) { MmUnmapIoSpace((PVOID)m_Mmio, (SIZE_T)m_BarMmio.Length); m_Mmio = NULL; }
    m_GartReady = FALSE;
    ExReleaseFastMutex(&m_Lock);
}

BOOLEAN BC250_GPU::ReadDeveloperMode()
{
    PAGED_CODE();
    HANDLE hKey = NULL;
    if (!m_pPdo || !NT_SUCCESS(IoOpenDeviceRegistryKey(m_pPdo, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &hKey))) return FALSE;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"DeveloperMode");
    UCHAR Buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = {0};
    ULONG Len = 0;
    BOOLEAN On = FALSE;
    if (NT_SUCCESS(ZwQueryValueKey(hKey, &Name, KeyValuePartialInformation, Buf, sizeof(Buf), &Len)))
    {
        KEY_VALUE_PARTIAL_INFORMATION* p = (KEY_VALUE_PARTIAL_INFORMATION*)Buf;
        if (p->Type == REG_DWORD && p->DataLength == sizeof(ULONG)) On = (*(ULONG*)p->Data) != 0;
    }
    ZwClose(hKey);
    return On;
}

VOID BC250_GPU::ReadMemoryLayout()
{
    PAGED_CODE();
    ULONG Base = Rreg(BC250_GCMC_VM_FB_LOCATION_BASE) & BC250_GCMC_VM_FB_LOCATION_BASE__FB_BASE_MASK;
    ULONG Top  = Rreg(BC250_GCMC_VM_FB_LOCATION_TOP) & BC250_GCMC_VM_FB_LOCATION_TOP__FB_TOP_MASK;
    m_FbStart = (ULONGLONG)Base << 24;
    m_FbEnd   = ((ULONGLONG)Top << 24) | 0xFFFFFFull;
    m_VramBasePhys = (ULONGLONG)Rreg(BC250_GCMC_VM_FB_OFFSET) << 24;
    m_VramSizeMb = Rreg(BC250_RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE);
}

NTSTATUS BC250_GPU::SmuSend(ULONG Msg, ULONG Arg, bc_u32* pResp, bc_u32* pOut)
{
    PAGED_CODE();
    *pResp = 0; *pOut = 0;
    if (!m_Mmio) return STATUS_DEVICE_NOT_READY;

    // smu_cmn: wait for the previous message, clear response, write arg, write message, poll.
    ULONG i;
    for (i = 0; i < 100000 && Rreg(BC250_MP1_SMN_C2PMSG_90) == 0; ++i) KeStallExecutionProcessor(1);
    Wreg(BC250_MP1_SMN_C2PMSG_90, 0);
    Wreg(BC250_MP1_SMN_C2PMSG_82, Arg);
    Wreg(BC250_MP1_SMN_C2PMSG_66, Msg);
    ULONG Resp = 0;
    for (i = 0; i < 200000; ++i)
    {
        Resp = Rreg(BC250_MP1_SMN_C2PMSG_90);
        if (Resp) break;
        KeStallExecutionProcessor(1);
    }
    *pResp = Resp;
    if (!Resp) return STATUS_IO_TIMEOUT;
    *pOut = Rreg(BC250_MP1_SMN_C2PMSG_82);
    return (Resp == PPSMC_Result_OK) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

#pragma code_seg(push)
#pragma code_seg()
VOID BC250_GPU::HdpFlush()
{
    Wreg(BC250_BIF_BX_PF_HDP_MEM_COHERENCY_FLUSH_CNTL, 0);
    (void)Rreg(BC250_BIF_BX_PF_HDP_MEM_COHERENCY_FLUSH_CNTL);
}
#pragma code_seg(pop)

#pragma code_seg(push)
#pragma code_seg()
VOID BC250_GPU::HdpInvalidate()
{
    Wreg(BC250_HDP_READ_CACHE_INVALIDATE, 1);
    (void)Rreg(BC250_HDP_READ_CACHE_INVALIDATE);
}
#pragma code_seg(pop)

VOID BC250_GPU::FillInfo(BC250_INFO* p)
{
    PAGED_CODE();
    RtlZeroMemory(p, sizeof(*p));
    p->VendorId = m_VendorId; p->DeviceId = m_DeviceId; p->SubsysId = m_SubsysId; p->Revision = m_Revision;
    p->DeveloperMode = m_DeveloperMode;
    p->MmioMapped = m_Mmio != NULL;
    p->Vram = m_BarVram; p->Doorbell = m_BarDoorbell; p->Mmio = m_BarMmio;
    p->FramebufferPhys = (ULONGLONG)m_FbPhys.QuadPart; p->FramebufferSize = m_FbSize;
    if (!m_Mmio) return;

    ReadMemoryLayout();
    p->GcFbStart = m_FbStart; p->GcFbEnd = m_FbEnd; p->GcFbOffset = m_VramBasePhys;
    p->MmFbStart = (ULONGLONG)(Rreg(BC250_MMMC_VM_FB_LOCATION_BASE) & 0xFFFFFF) << 24;
    p->MmFbEnd   = ((ULONGLONG)(Rreg(BC250_MMMC_VM_FB_LOCATION_TOP) & 0xFFFFFF) << 24) | 0xFFFFFF;
    p->GcSysApertureLow  = (ULONGLONG)Rreg(BC250_GCMC_VM_SYSTEM_APERTURE_LOW_ADDR) << 18;
    p->GcSysApertureHigh = (ULONGLONG)Rreg(BC250_GCMC_VM_SYSTEM_APERTURE_HIGH_ADDR) << 18;
    p->MmSysApertureLow  = (ULONGLONG)Rreg(BC250_MMMC_VM_SYSTEM_APERTURE_LOW_ADDR) << 18;
    p->MmSysApertureHigh = (ULONGLONG)Rreg(BC250_MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR) << 18;
    p->VramSizeMb   = m_VramSizeMb;
    p->GrbmStatus   = Rreg(BC250_GRBM_STATUS);
    p->GrbmStatus2  = Rreg(BC250_GRBM_STATUS2);
    p->CpStat       = Rreg(BC250_CP_STAT);
    p->RlcStat      = Rreg(BC250_RLC_STAT);
    p->GbAddrConfig = Rreg(BC250_GB_ADDR_CONFIG);
    p->SdmaStatus   = Rreg(BC250_SDMA0_STATUS_REG);
    p->SdmaF32Cntl  = Rreg(BC250_SDMA0_F32_CNTL);
    p->IhRbCntl     = Rreg(BC250_IH_RB_CNTL);
    p->ThermalRaw   = Rreg(BC250_CG_MULT_THERMAL_STATUS);

    bc_u32 Resp, Out;
    if (NT_SUCCESS(SmuSend(PPSMC_MSG_GetSmuVersion, 0, &Resp, &Out))) p->SmuVersion = Out;
    if (NT_SUCCESS(SmuSend(PPSMC_MSG_GetGfxFrequency, 0, &Resp, &Out))) p->GfxClkMhz = Out;

    p->ScratchMc = m_Scratch ? m_ScratchMc : 0;
    p->ScratchSize = m_Scratch ? BC250_SCRATCH_SIZE : 0;
    p->GartStart = m_GartStart; p->GartEnd = m_GartEnd;
    p->GartReady = m_GartReady; p->IhReady = m_IhReady; p->SdmaReady = m_SdmaReady; p->CpReady = m_CpReady;
    p->InterruptCount = (ULONGLONG)m_IntCount;
}

NTSTATUS BC250_GPU::MapScratch()
{
    PAGED_CODE();
    if (m_Scratch) return STATUS_SUCCESS;
    if (!m_Mmio || !m_BarVram.Length) return STATUS_DEVICE_NOT_READY;
    ReadMemoryLayout();
    if (m_FbEnd <= m_FbStart) return STATUS_DEVICE_CONFIGURATION_ERROR;   // VBIOS did not set FB location

    ULONGLONG End = BC250_SCRATCH_OFFSET + BC250_SCRATCH_SIZE;
    if (End > m_BarVram.Length) return STATUS_INSUFFICIENT_RESOURCES;          // BAR0 too small
    if (m_VramSizeMb && End > (ULONGLONG)m_VramSizeMb << 20) return STATUS_INSUFFICIENT_RESOURCES;

    // must not overlap the GOP framebuffer
    ULONGLONG FbOff = (ULONGLONG)m_FbPhys.QuadPart - m_BarVram.PhysAddr;
    if ((ULONGLONG)m_FbPhys.QuadPart >= m_BarVram.PhysAddr &&
        FbOff < End && FbOff + m_FbSize > BC250_SCRATCH_OFFSET)
        return STATUS_CONFLICTING_ADDRESSES;

    PHYSICAL_ADDRESS Pa; Pa.QuadPart = (LONGLONG)(m_BarVram.PhysAddr + BC250_SCRATCH_OFFSET);
    m_Scratch = (volatile UCHAR*)MmMapIoSpaceEx(Pa, (SIZE_T)BC250_SCRATCH_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!m_Scratch) return STATUS_INSUFFICIENT_RESOURCES;
    m_ScratchMc = m_FbStart + BC250_SCRATCH_OFFSET;
    for (ULONG o = 0; o < BC250_SCR_TEST; o += 4) ScrWrite(o, 0);
    HdpFlush();
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------------------------
// M3: GCVM (gfxhub_v2_0_gart_enable, simplified to VMID0)
// ----------------------------------------------------------------------------------------------

NTSTATUS BC250_GPU::TlbFlush()
{
    PAGED_CODE();
    ULONG Req = (1u << BC250_GCVM_INVALIDATE_ENG17_REQ__PER_VMID_INVALIDATE_REQ__SHIFT) |   // VMID0
                BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PTES_MASK |
                BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE0_MASK |
                BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE1_MASK |
                BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PDE2_MASK |
                BC250_GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L1_PTES_MASK;
    Wreg(BC250_GCVM_INVALIDATE_ENG17_REQ, Req);
    for (ULONG i = 0; i < 100000; ++i)
    {
        if (Rreg(BC250_GCVM_INVALIDATE_ENG17_ACK) & 1u) return STATUS_SUCCESS;
        KeStallExecutionProcessor(1);
    }
    return STATUS_IO_TIMEOUT;
}

NTSTATUS BC250_GPU::GartInit()
{
    PAGED_CODE();
    NTSTATUS St = MapScratch();
    if (!NT_SUCCESS(St)) return St;

    // GART placement: below the FB if there is room, otherwise above it (4 GiB aligned)
    if (m_FbStart >= BC250_GART_SIZE) m_GartStart = 0;
    else m_GartStart = (m_FbEnd + 1 + 0xFFFFFFFFull) & ~0xFFFFFFFFull;
    m_GartEnd = m_GartStart + BC250_GART_SIZE - 1;

    // empty page table (invalid PTEs fault to the dummy page)
    for (ULONG o = 0; o < (ULONG)(BC250_GART_SIZE >> 12) * 8; o += 4) ScrWrite(BC250_SCR_GART_PT + o, 0);
    HdpFlush();

    ULONGLONG PtPhys    = McToPhys(ScrMc(BC250_SCR_GART_PT));
    ULONGLONG DummyPhys = McToPhys(ScrMc(BC250_SCR_DUMMY));
    ULONGLONG Pd = PtPhys | PDE_MTYPE_VG10(MTYPE_UC);

    // gart aperture
    Wreg(BC250_GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, (ULONG)Pd);
    Wreg(BC250_GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, (ULONG)(Pd >> 32));
    Wreg(BC250_GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, (ULONG)(m_GartStart >> 12));
    Wreg(BC250_GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, (ULONG)(m_GartStart >> 44));
    Wreg(BC250_GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, (ULONG)(m_GartEnd >> 12));
    Wreg(BC250_GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, (ULONG)(m_GartEnd >> 44));

    // system aperture = FB, AGP disabled
    Wreg(BC250_GCMC_VM_AGP_BASE, 0);
    Wreg(BC250_GCMC_VM_AGP_BOT, 0xFFFFFF);
    Wreg(BC250_GCMC_VM_AGP_TOP, 0);
    Wreg(BC250_GCMC_VM_SYSTEM_APERTURE_LOW_ADDR, (ULONG)(m_FbStart >> 18));
    Wreg(BC250_GCMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (ULONG)(m_FbEnd >> 18));
    Wreg(BC250_GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB, (ULONG)(DummyPhys >> 12));
    Wreg(BC250_GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB, (ULONG)(DummyPhys >> 44));
    Wreg(BC250_GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, (ULONG)(DummyPhys >> 12));
    Wreg(BC250_GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, (ULONG)(DummyPhys >> 44));

    // L1 TLB
    ULONG t = Rreg(BC250_GCMC_VM_MX_L1_TLB_CNTL);
    t |= BC250_GCMC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK | BC250_GCMC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL_MASK;
    t = SETF(t, BC250_GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE_MASK, 3);
    t &= ~BC250_GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS_MASK;
    t = SETF(t, BC250_GCMC_VM_MX_L1_TLB_CNTL__MTYPE_MASK, MTYPE_UC);
    Wreg(BC250_GCMC_VM_MX_L1_TLB_CNTL, t);

    // L2 cache
    t = Rreg(BC250_GCVM_L2_CNTL);
    t |= BC250_GCVM_L2_CNTL__ENABLE_L2_CACHE_MASK | BC250_GCVM_L2_CNTL__ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY_MASK;
    t &= ~BC250_GCVM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING_MASK;
    t = SETF(t, BC250_GCVM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE_MASK, 1);
    Wreg(BC250_GCVM_L2_CNTL, t);
    Rmw(BC250_GCVM_L2_CNTL2, 0, BC250_GCVM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS_MASK | BC250_GCVM_L2_CNTL2__INVALIDATE_L2_CACHE_MASK);

    // VMID0: flat (depth 0) page table
    t = Rreg(BC250_GCVM_CONTEXT0_CNTL);
    t |= BC250_GCVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK;
    t &= ~(BC250_GCVM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH_MASK | BC250_GCVM_CONTEXT0_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK);
    Wreg(BC250_GCVM_CONTEXT0_CNTL, t);

    for (ULONG e = 0; e < 18; ++e)
    {
        Wreg(BC250_GCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32 + e * 2, 0xFFFFFFFF);
        Wreg(BC250_GCVM_INVALIDATE_ENG0_ADDR_RANGE_HI32 + e * 2, 0x1F);
    }
    St = TlbFlush();
    m_GartReady = NT_SUCCESS(St);
    return St;
}

NTSTATUS BC250_GPU::GartMapTestPage()
{
    PAGED_CODE();
    if (!m_GartReady) return STATUS_INVALID_DEVICE_STATE;
    if (!m_TestPage)
    {
        PHYSICAL_ADDRESS Lo, Hi, Bound;
        Lo.QuadPart = 0; Hi.QuadPart = (LONGLONG)0x0000FFFFFFFFFFFFull; Bound.QuadPart = 0;
        m_TestPage = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, Lo, Hi, Bound, MmCached);
        if (!m_TestPage) return STATUS_INSUFFICIENT_RESOURCES;
        m_TestPagePhys = MmGetPhysicalAddress(m_TestPage);
    }
    RtlZeroMemory(m_TestPage, PAGE_SIZE);
    ULONGLONG Pte = ((ULONGLONG)m_TestPagePhys.QuadPart & 0x0000FFFFFFFFF000ull) |
                    PTE_VALID | PTE_SYSTEM | PTE_SNOOPED | PTE_EXECUTABLE | PTE_READABLE | PTE_WRITEABLE |
                    PTE_MTYPE_NV10(MTYPE_UC);
    ScrWrite(BC250_SCR_GART_PT, (ULONG)Pte);
    ScrWrite(BC250_SCR_GART_PT + 4, (ULONG)(Pte >> 32));
    HdpFlush();
    return TlbFlush();
}

// ----------------------------------------------------------------------------------------------
// M2: IH ring (navi10_ih_irq_init, ring 0 only, no doorbell, no MSI)
// ----------------------------------------------------------------------------------------------

VOID BC250_GPU::IhDisable()
{
    PAGED_CODE();
    ULONG t = Rreg(BC250_IH_RB_CNTL);
    t &= ~(BC250_IH_RB_CNTL__RB_ENABLE_MASK | BC250_IH_RB_CNTL__ENABLE_INTR_MASK);
    Wreg(BC250_IH_RB_CNTL, t);
    m_IhReady = FALSE;
}

NTSTATUS BC250_GPU::IhInit()
{
    PAGED_CODE();
    NTSTATUS St = MapScratch();
    if (!NT_SUCCESS(St)) return St;
    IhDisable();

    ULONGLONG DummyPhys = McToPhys(ScrMc(BC250_SCR_DUMMY));
    Wreg(BC250_INTERRUPT_CNTL2, (ULONG)(DummyPhys >> 8));

    for (ULONG o = 0; o < BC250_IH_RING_SIZE; o += 4) ScrWrite(BC250_SCR_IH_RING + o, 0);
    ScrWrite(BC250_SCR_IH_WPTR, 0);
    HdpFlush();

    ULONGLONG Ring = ScrMc(BC250_SCR_IH_RING);
    Wreg(BC250_IH_RB_BASE, (ULONG)(Ring >> 8));
    Wreg(BC250_IH_RB_BASE_HI, (ULONG)(Ring >> 40) & 0xFF);

    ULONG t = Rreg(BC250_IH_RB_CNTL);
    t = SETF(t, BC250_IH_RB_CNTL__MC_SPACE_MASK, 4);
    t |= BC250_IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK | BC250_IH_RB_CNTL__WPTR_OVERFLOW_ENABLE_MASK |
         BC250_IH_RB_CNTL__WPTR_WRITEBACK_ENABLE_MASK | BC250_IH_RB_CNTL__MC_SNOOP_MASK;
    t &= ~(0x1Fu << BC250_IH_RB_CNTL__RB_SIZE__SHIFT);
    t |= Log2(BC250_IH_RING_SIZE / 4) << BC250_IH_RB_CNTL__RB_SIZE__SHIFT;
    // navi10_ih_enable_ring: RPTR_REARM = msi_enabled
    if (m_Msi) t |= BC250_IH_RB_CNTL__RPTR_REARM_MASK; else t &= ~BC250_IH_RB_CNTL__RPTR_REARM_MASK;
    Wreg(BC250_IH_RB_CNTL, t);

    ULONGLONG Wb = ScrMc(BC250_SCR_IH_WPTR);
    Wreg(BC250_IH_RB_WPTR_ADDR_LO, (ULONG)Wb);
    Wreg(BC250_IH_RB_WPTR_ADDR_HI, (ULONG)(Wb >> 32) & 0xFFFF);
    Wreg(BC250_IH_RB_WPTR, 0);
    Wreg(BC250_IH_RB_RPTR, 0);
    Wreg(BC250_IH_DOORBELL_RPTR, 0);
    m_IhRptr = 0;

    // enable (navi10_ih_toggle_ring_interrupts)
    t = Rreg(BC250_IH_RB_CNTL);
    t |= BC250_IH_RB_CNTL__RB_ENABLE_MASK | BC250_IH_RB_CNTL__ENABLE_INTR_MASK | BC250_IH_RB_CNTL__RB_GPU_TS_ENABLE_MASK;
    t &= ~BC250_IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK;
    m_IhReady = TRUE;   // ISR may run from here on
    Wreg(BC250_IH_RB_CNTL, t);
    Wreg(BC250_IH_RB_CNTL, t | BC250_IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK);
    Wreg(BC250_IH_RB_CNTL, t);
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------------------------
// M4: SDMA0 (sdma_v5_0_start with AMDGPU_FW_LOAD_DIRECT, bare metal, no doorbell)
// ----------------------------------------------------------------------------------------------

NTSTATUS BC250_GPU::FwUpload(const BC250_FW* p)
{
    PAGED_CODE();
    if (p->Engine >= BC250_FW_COUNT) return STATUS_INVALID_PARAMETER;
    if (p->TotalSize < 64 || p->TotalSize > 1024 * 1024) return STATUS_INVALID_PARAMETER;
    if (p->ChunkSize > BC250_FW_CHUNK || p->Offset > p->TotalSize || p->ChunkSize > p->TotalSize - p->Offset)
        return STATUS_INVALID_PARAMETER;
    UCHAR** ppBuf = (p->Engine == BC250_FW_SDMA0) ? &m_Fw : &m_FwCp[p->Engine];
    ULONG*  pSize = (p->Engine == BC250_FW_SDMA0) ? &m_FwSize : &m_FwCpSize[p->Engine];
    if (p->Offset == 0)
    {
        if (*ppBuf) ExFreePoolWithTag(*ppBuf, BC250_TAG);
        *ppBuf = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, p->TotalSize, BC250_TAG);
        if (!*ppBuf) { *pSize = 0; return STATUS_INSUFFICIENT_RESOURCES; }
        *pSize = p->TotalSize;
    }
    if (!*ppBuf || *pSize != p->TotalSize) return STATUS_INVALID_DEVICE_STATE;
    RtlCopyMemory(*ppBuf + p->Offset, p->Data, p->ChunkSize);
    return STATUS_SUCCESS;
}

VOID BC250_GPU::SdmaHalt()
{
    PAGED_CODE();
    Rmw(BC250_SDMA0_GFX_RB_CNTL, BC250_SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK, 0);
    Rmw(BC250_SDMA0_GFX_IB_CNTL, BC250_SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK, 0);
    Rmw(BC250_SDMA0_F32_CNTL, 0, BC250_SDMA0_F32_CNTL__HALT_MASK);
    m_SdmaReady = FALSE;
}

NTSTATUS BC250_GPU::SdmaStart()
{
    PAGED_CODE();
    if (!m_GartReady) return STATUS_INVALID_DEVICE_STATE;     // UTCL1 needs the system aperture
    if (!m_Fw) return STATUS_INVALID_DEVICE_STATE;

    // amdgpu common_firmware_header
    ULONG UcodeVersion = *(ULONG*)(m_Fw + 16);
    ULONG UcodeBytes   = *(ULONG*)(m_Fw + 20);
    ULONG UcodeOffset  = *(ULONG*)(m_Fw + 24);
    if (UcodeOffset >= m_FwSize || UcodeBytes > m_FwSize - UcodeOffset || (UcodeBytes & 3) || !UcodeBytes)
        return STATUS_INVALID_IMAGE_FORMAT;

    SdmaHalt();

    // golden settings (golden_settings_sdma_cyan_skillfish, SDMA0 entries)
    Rmw(BC250_SDMA0_CHICKEN_BITS, 0xffbf1f0f, 0x03ab0107 & 0xffbf1f0f);
    Rmw(BC250_SDMA0_GB_ADDR_CONFIG, 0x001877ff, 0x00000044);
    Rmw(BC250_SDMA0_GB_ADDR_CONFIG_READ, 0x001877ff, 0x00000044);
    Rmw(BC250_SDMA0_GFX_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000);
    Rmw(BC250_SDMA0_UTCL1_PAGE, 0x007fffff, 0x004c5c00);

    // direct microcode load, unless the PSP already loaded it (cyan_skillfish2 protects the ucode port)
    if (!(m_PspLoaded & (1u << BC250_FW_SDMA0)))
    {
        const ULONG* Code = (const ULONG*)(m_Fw + UcodeOffset);
        Wreg(BC250_SDMA0_UCODE_ADDR, 0);
        for (ULONG i = 0; i < UcodeBytes / 4; ++i) Wreg(BC250_SDMA0_UCODE_DATA, Code[i]);
        Wreg(BC250_SDMA0_UCODE_ADDR, UcodeVersion);
    }

    // unhalt + context switch (sdma_v5_0_enable / ctx_switch_enable)
    Rmw(BC250_SDMA0_F32_CNTL, BC250_SDMA0_F32_CNTL__HALT_MASK, 0);
    Rmw(BC250_SDMA0_CNTL, 0, BC250_SDMA0_CNTL__AUTO_CTXSW_ENABLE_MASK);

    // ring (sdma_v5_0_gfx_resume_instance)
    for (ULONG o = 0; o < BC250_SDMA_RING_SIZE; o += 4) ScrWrite(BC250_SCR_SDMA_RING + o, 0);
    for (ULONG o = 0; o < 0x1000; o += 4) ScrWrite(BC250_SCR_WB + o, 0);
    HdpFlush();

    Wreg(BC250_SDMA0_SEM_WAIT_FAIL_TIMER_CNTL, 0);
    ULONG Rb = Rreg(BC250_SDMA0_GFX_RB_CNTL);
    Rb = SETF(Rb, BC250_SDMA0_GFX_RB_CNTL__RB_SIZE_MASK, Log2(BC250_SDMA_RING_SIZE / 4));
    Wreg(BC250_SDMA0_GFX_RB_CNTL, Rb);
    Wreg(BC250_SDMA0_GFX_RB_RPTR, 0);
    Wreg(BC250_SDMA0_GFX_RB_RPTR_HI, 0);
    Wreg(BC250_SDMA0_GFX_RB_WPTR, 0);
    Wreg(BC250_SDMA0_GFX_RB_WPTR_HI, 0);

    ULONGLONG WptrShadow = ScrMc(BC250_SCR_WB + 8);
    Wreg(BC250_SDMA0_GFX_RB_WPTR_POLL_ADDR_LO, (ULONG)WptrShadow);
    Wreg(BC250_SDMA0_GFX_RB_WPTR_POLL_ADDR_HI, (ULONG)(WptrShadow >> 32));
    Rmw(BC250_SDMA0_GFX_RB_WPTR_POLL_CNTL, BC250_SDMA0_GFX_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK, 0);  // register wptr

    ULONGLONG RptrWb = ScrMc(BC250_SCR_WB);
    Wreg(BC250_SDMA0_GFX_RB_RPTR_ADDR_HI, (ULONG)(RptrWb >> 32));
    Wreg(BC250_SDMA0_GFX_RB_RPTR_ADDR_LO, (ULONG)RptrWb & 0xFFFFFFFC);
    Rb |= BC250_SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK;

    ULONGLONG Ring = ScrMc(BC250_SCR_SDMA_RING);
    Wreg(BC250_SDMA0_GFX_RB_BASE, (ULONG)(Ring >> 8));
    Wreg(BC250_SDMA0_GFX_RB_BASE_HI, (ULONG)(Ring >> 40));
    m_SdmaWptr = 0;

    Wreg(BC250_SDMA0_GFX_MINOR_PTR_UPDATE, 1);
    Wreg(BC250_SDMA0_GFX_RB_WPTR, 0);
    Wreg(BC250_SDMA0_GFX_RB_WPTR_HI, 0);
    Rmw(BC250_SDMA0_GFX_DOORBELL, BC250_SDMA0_GFX_DOORBELL__ENABLE_MASK, 0);
    Wreg(BC250_SDMA0_GFX_MINOR_PTR_UPDATE, 0);

    ULONG c = Rreg(BC250_SDMA0_CNTL);
    c |= BC250_SDMA0_CNTL__UTC_L1_ENABLE_MASK | BC250_SDMA0_CNTL__MIDCMD_PREEMPT_ENABLE_MASK | BC250_SDMA0_CNTL__TRAP_ENABLE_MASK;
    Wreg(BC250_SDMA0_CNTL, c);
    ULONG u = Rreg(BC250_SDMA0_UTCL1_CNTL);
    u = SETF(u, BC250_SDMA0_UTCL1_CNTL__RESP_MODE_MASK, 3);
    u = SETF(u, BC250_SDMA0_UTCL1_CNTL__REDO_DELAY_MASK, 9);
    Wreg(BC250_SDMA0_UTCL1_CNTL, u);

    Rmw(BC250_SDMA0_F32_CNTL, BC250_SDMA0_F32_CNTL__HALT_MASK, 0);
    Rb |= BC250_SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK;
    Wreg(BC250_SDMA0_GFX_RB_CNTL, Rb);
    Rmw(BC250_SDMA0_GFX_IB_CNTL, 0, BC250_SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK);

    m_SdmaReady = TRUE;
    return STATUS_SUCCESS;
}

#pragma code_seg(push)
#pragma code_seg()
VOID BC250_GPU::SdmaWrite(ULONG Dw)
{
    ScrWrite(BC250_SCR_SDMA_RING + (m_SdmaWptr % (BC250_SDMA_RING_SIZE / 4)) * 4, Dw);
    ++m_SdmaWptr;
}
#pragma code_seg(pop)

#pragma code_seg(push)
#pragma code_seg()
VOID BC250_GPU::SdmaCommit()
{
    HdpFlush();
    ULONGLONG Bytes = (ULONGLONG)m_SdmaWptr * 4;   // wptr is a byte offset, wraps with RB_SIZE
    Wreg(BC250_SDMA0_GFX_RB_WPTR, (ULONG)Bytes);
    Wreg(BC250_SDMA0_GFX_RB_WPTR_HI, (ULONG)(Bytes >> 32));
}
#pragma code_seg(pop)

BOOLEAN BC250_GPU::WaitScratch(ULONG Off, ULONG Expect, ULONG TimeoutUs, bc_u32* pSeen)
{
    PAGED_CODE();
    ULONG v = 0;
    for (ULONG i = 0; i < TimeoutUs; ++i)
    {
        HdpInvalidate();
        v = ScrRead(Off);
        if (v == Expect) { *pSeen = v; return TRUE; }
        KeStallExecutionProcessor(1);
    }
    *pSeen = v;
    return FALSE;
}

NTSTATUS BC250_GPU::SdmaTest(BC250_TEST* t)
{
    PAGED_CODE();
    RtlZeroMemory(t, sizeof(*t));
    if (!m_SdmaReady) return STATUS_INVALID_DEVICE_STATE;
    LARGE_INTEGER T0 = KeQueryPerformanceCounter(NULL), Freq;

    // 1) WRITE_LINEAR into VRAM
    t->Step = 1;
    const ULONG Slot = BC250_SCR_WB + 0x100;
    ScrWrite(Slot, 0xCAFEDEAD);
    HdpFlush();
    ULONGLONG Dst = ScrMc(Slot);
    SdmaWrite(SDMA_HDR(SDMA_OP_WRITE, SDMA_SUBOP_WRITE_LINEAR));
    SdmaWrite((ULONG)Dst);
    SdmaWrite((ULONG)(Dst >> 32));
    SdmaWrite(0);               // COUNT = dwords - 1
    SdmaWrite(0xDEADBEEF);
    SdmaCommit();
    t->Expected = 0xDEADBEEF;
    BOOLEAN Ok = WaitScratch(Slot, 0xDEADBEEF, 100000, &t->Observed);

    t->Rptr = Rreg(BC250_SDMA0_GFX_RB_RPTR);
    t->Wptr = Rreg(BC250_SDMA0_GFX_RB_WPTR);
    t->SdmaStatus = Rreg(BC250_SDMA0_STATUS_REG);
    t->FaultStatus = Rreg(BC250_GCVM_L2_PROTECTION_FAULT_STATUS);
    if (!Ok) goto done;

    // 2) WRITE_LINEAR through GART into a system page (validates M3)
    if (m_TestPage)
    {
        t->Step = 2;
        volatile ULONG* Sys = (volatile ULONG*)m_TestPage;
        Sys[0] = 0;
        SdmaWrite(SDMA_HDR(SDMA_OP_WRITE, SDMA_SUBOP_WRITE_LINEAR));
        SdmaWrite((ULONG)m_GartStart);
        SdmaWrite((ULONG)(m_GartStart >> 32));
        SdmaWrite(0);
        SdmaWrite(0x600DF00D);
        SdmaCommit();
        t->Expected = 0x600DF00D;
        Ok = FALSE;
        for (ULONG i = 0; i < 100000 && !Ok; ++i) { if (Sys[0] == 0x600DF00D) Ok = TRUE; else KeStallExecutionProcessor(1); }
        t->Observed = Sys[0];
        t->FaultStatus = Rreg(BC250_GCVM_L2_PROTECTION_FAULT_STATUS);
        if (!Ok) goto done;
    }

    // 3) FENCE
    t->Step = 3;
    {
        const ULONG FenceSlot = Slot + 4;
        ScrWrite(FenceSlot, 0);
        HdpFlush();
        ULONGLONG F = ScrMc(FenceSlot);
        SdmaWrite(SDMA_HDR(SDMA_OP_FENCE, 0) | (3u << 16));   // MTYPE=UC: fence must reach memory
        SdmaWrite((ULONG)F);
        SdmaWrite((ULONG)(F >> 32));
        SdmaWrite(0x00000250);
        SdmaCommit();
        t->Expected = 0x250;
        Ok = WaitScratch(FenceSlot, 0x250, 100000, &t->Observed);
        if (!Ok) goto done;
    }

    // 4) TRAP -> IH -> ISR (validates M2)
    if (m_IhReady)
    {
        t->Step = 4;
        t->TrapsBefore = (ULONG)m_SdmaTraps;
        SdmaWrite(SDMA_HDR(SDMA_OP_TRAP, 0));
        SdmaWrite(0);           // INT_CONTEXT
        SdmaCommit();
        Ok = FALSE;
        for (ULONG i = 0; i < 100000 && !Ok; ++i) { if ((ULONG)m_SdmaTraps != t->TrapsBefore) Ok = TRUE; else KeStallExecutionProcessor(1); }
        t->TrapsAfter = (ULONG)m_SdmaTraps;
        t->Expected = t->TrapsBefore + 1;
        t->Observed = t->TrapsAfter;
        if (!Ok) goto done;
    }
    t->Step = 100;   // all passed

done:
    LARGE_INTEGER T1 = KeQueryPerformanceCounter(&Freq);
    t->ElapsedUs = (ULONG)(((T1.QuadPart - T0.QuadPart) * 1000000) / (Freq.QuadPart ? Freq.QuadPart : 1));
    t->Rptr = Rreg(BC250_SDMA0_GFX_RB_RPTR);
    t->Wptr = Rreg(BC250_SDMA0_GFX_RB_WPTR);
    t->SdmaStatus = Rreg(BC250_SDMA0_STATUS_REG);
    return Ok ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

// ----------------------------------------------------------------------------------------------
// M5: command submission (indirect buffer + fence + trap), the path a render KMD's
// DxgkDdiSubmitCommand / BuildPagingBuffer would use.
// ----------------------------------------------------------------------------------------------

#define SDMA_OP_COPY        1
#define SDMA_OP_INDIRECT    4
#define SDMA_OP_CONST_FILL  11
#define SDMA_FILLSIZE_DWORD (2u << 30)

NTSTATUS BC250_GPU::SubmitIb(const ULONG* pIb, ULONG Count, ULONG* pFence, ULONG* pUs)
{
    PAGED_CODE();
    *pFence = 0; *pUs = 0;
    if (!m_SdmaReady) return STATUS_INVALID_DEVICE_STATE;
    if (!Count || Count > BC250_IB_MAX_DWORDS) return STATUS_INVALID_PARAMETER;

    // copy the IB into VRAM (32-byte aligned)
    for (ULONG i = 0; i < Count; ++i) ScrWrite(BC250_SCR_IB + i * 4, pIb[i]);
    ULONGLONG IbMc = ScrMc(BC250_SCR_IB);
    ULONG FenceValue = ++m_SdmaFence;
    ScrWrite(BC250_SCR_FENCE, 0);

    // pad so the 6-dword INDIRECT packet ends on an 8-dword boundary (sdma_v5_0_ring_emit_ib)
    ULONG Nops = (2 - (m_SdmaWptr & 7)) & 7;
    for (ULONG i = 0; i < Nops; ++i) SdmaWrite(SDMA_HDR(SDMA_OP_NOP, 0));
    SdmaWrite(SDMA_HDR(SDMA_OP_INDIRECT, 0) | (0u << 16));   // VMID 0
    SdmaWrite((ULONG)IbMc & 0xFFFFFFE0);
    SdmaWrite((ULONG)(IbMc >> 32));
    SdmaWrite(Count);
    SdmaWrite(0);                                            // CSA (unused, no preemption)
    SdmaWrite(0);

    ULONGLONG F = ScrMc(BC250_SCR_FENCE);
    SdmaWrite(SDMA_HDR(SDMA_OP_FENCE, 0) | (3u << 16));   // MTYPE=UC: fence must reach memory
    SdmaWrite((ULONG)F);
    SdmaWrite((ULONG)(F >> 32));
    SdmaWrite(FenceValue);
    if (m_IhReady) { SdmaWrite(SDMA_HDR(SDMA_OP_TRAP, 0)); SdmaWrite(0); }

    LARGE_INTEGER T0 = KeQueryPerformanceCounter(NULL), Freq;
    SdmaCommit();
    bc_u32 Seen = 0;
    BOOLEAN Ok = WaitScratch(BC250_SCR_FENCE, FenceValue, 500000, &Seen);
    LARGE_INTEGER T1 = KeQueryPerformanceCounter(&Freq);
    *pUs = (ULONG)(((T1.QuadPart - T0.QuadPart) * 1000000) / (Freq.QuadPart ? Freq.QuadPart : 1));
    *pFence = Seen;
    return Ok ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

NTSTATUS BC250_GPU::FillRect(BC250_RECT* r)
{
    PAGED_CODE();
    if (!m_SdmaReady) return STATUS_INVALID_DEVICE_STATE;
    if (!m_FbPitch || !m_FbWidth || !m_FbHeight) return STATUS_DEVICE_NOT_READY;
    if (r->X >= m_FbWidth || r->Y >= m_FbHeight || !r->Width || !r->Height) return STATUS_INVALID_PARAMETER;
    ULONG W = min(r->Width, m_FbWidth - r->X);
    ULONG H = min(r->Height, m_FbHeight - r->Y);

    // scanout GPU address: same offset inside VRAM as inside the BAR0 aperture
    if ((ULONGLONG)m_FbPhys.QuadPart < m_BarVram.PhysAddr) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONGLONG FbMc = m_FbStart + ((ULONGLONG)m_FbPhys.QuadPart - m_BarVram.PhysAddr);

    // one CONSTANT_FILL (5 dwords) per row; split into IBs of up to 200 rows
    ULONG IbBuf[500];
    ULONG Packets = 0, Fence = 0, Us = 0, TotalUs = 0;
    for (ULONG Row = 0; Row < H; )
    {
        ULONG n = 0;
        for (; Row < H && n + 5 <= ARRAYSIZE(IbBuf); ++Row)
        {
            ULONGLONG Dst = FbMc + (ULONGLONG)(r->Y + Row) * m_FbPitch + (ULONGLONG)r->X * 4;
            IbBuf[n++] = SDMA_HDR(SDMA_OP_CONST_FILL, 0) | SDMA_FILLSIZE_DWORD;
            IbBuf[n++] = (ULONG)Dst;
            IbBuf[n++] = (ULONG)(Dst >> 32);
            IbBuf[n++] = r->Color;
            IbBuf[n++] = W * 4 - 1;           // byte count - 1
            ++Packets;
        }
        NTSTATUS St = SubmitIb(IbBuf, n, &Fence, &Us);
        TotalUs += Us;
        if (!NT_SUCCESS(St)) { r->Fence = Fence; r->ElapsedUs = TotalUs; r->Packets = Packets; return St; }
    }
    r->Fence = Fence; r->ElapsedUs = TotalUs; r->Packets = Packets;
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------------------------
// Escape dispatcher
// ----------------------------------------------------------------------------------------------

static BOOLEAN SmuMsgIsReadOnly(ULONG Msg)
{
    switch (Msg)
    {
    case PPSMC_MSG_TestMessage: case PPSMC_MSG_GetSmuVersion: case PPSMC_MSG_GetDriverIfVersion:
    case PPSMC_MSG_QueryGfxclk: case PPSMC_MSG_QueryVddcrSocClock: case PPSMC_MSG_QueryDfPstate:
    case PPSMC_MSG_QueryActiveWgp: case PPSMC_MSG_GetGfxFrequency: case PPSMC_MSG_GetGfxVid:
        return TRUE;
    }
    return FALSE;
}

static bc_i32 ToEsc(NTSTATUS St)
{
    if (NT_SUCCESS(St)) return BC250_OK;
    switch (St)
    {
    case STATUS_IO_TIMEOUT:              return BC250_E_TIMEOUT;
    case STATUS_INVALID_DEVICE_STATE:    return BC250_E_STATE;
    case STATUS_INSUFFICIENT_RESOURCES:  return BC250_E_NOMEM;
    case STATUS_INVALID_PARAMETER:
    case STATUS_INVALID_IMAGE_FORMAT:
    case STATUS_CONFLICTING_ADDRESSES:   return BC250_E_RANGE;
    case STATUS_DEVICE_NOT_READY:        return BC250_E_NOHW;
    default:                             return BC250_E_VERIFY;
    }
}

NTSTATUS BC250_GPU::Escape(VOID* pData, UINT Size)
{
    PAGED_CODE();
    if (!pData || Size < sizeof(BC250_ESC_HEADER)) return STATUS_INVALID_PARAMETER;
    BC250_ESCAPE* e = (BC250_ESCAPE*)pData;
    if (e->Header.Magic != BC250_ESC_MAGIC || e->Header.Version != BC250_ESC_VERSION) return STATUS_INVALID_PARAMETER;
    if (Size < sizeof(BC250_ESCAPE)) return STATUS_BUFFER_TOO_SMALL;

    BOOLEAN Dev = ReadDeveloperMode();        // PASSIVE_LEVEL; picks up registry changes without a reboot
    ExAcquireFastMutex(&m_Lock);
    m_DeveloperMode = Dev;
    const ULONG Cmd = e->Header.Command;
    const BOOLEAN NeedsDev = !(Cmd == BC250_CMD_GET_INFO || Cmd == BC250_CMD_IH_STATUS ||
                               (Cmd == BC250_CMD_SMU_MSG && SmuMsgIsReadOnly(e->u.Smu.Message)));
    bc_i32 R = BC250_OK;

    if (NeedsDev && !m_DeveloperMode) R = BC250_E_DENIED;
    else if (!m_Mmio && Cmd != BC250_CMD_GET_INFO) R = BC250_E_NOHW;
    else switch (Cmd)
    {
    case BC250_CMD_GET_INFO:
        FillInfo(&e->u.Info);
        break;
    case BC250_CMD_READ_REG:
        if ((ULONGLONG)e->u.Reg.Offset * 4 + 4 > m_BarMmio.Length) R = BC250_E_RANGE;
        else e->u.Reg.Value = Rreg(e->u.Reg.Offset);
        break;
    case BC250_CMD_WRITE_REG:
        if ((ULONGLONG)e->u.Reg.Offset * 4 + 4 > m_BarMmio.Length) R = BC250_E_RANGE;
        else { Wreg(e->u.Reg.Offset, e->u.Reg.Value); e->u.Reg.Value = Rreg(e->u.Reg.Offset); }
        break;
    case BC250_CMD_SMU_MSG:
    {
        NTSTATUS St = SmuSend(e->u.Smu.Message, e->u.Smu.Argument, &e->u.Smu.Response, &e->u.Smu.Output);
        R = (St == STATUS_IO_TIMEOUT) ? BC250_E_TIMEOUT : (NT_SUCCESS(St) ? BC250_OK : BC250_E_SMU);
        break;
    }
    case BC250_CMD_FW_UPLOAD:   R = ToEsc(FwUpload(&e->u.Fw)); break;
    case BC250_CMD_GART_INIT:   R = ToEsc(GartInit()); break;
    case BC250_CMD_GART_TEST:   R = ToEsc(GartMapTestPage()); break;
    case BC250_CMD_IH_INIT:     R = ToEsc(IhInit()); break;
    case BC250_CMD_IH_STATUS:
        e->u.Ih.InterruptCount = (ULONGLONG)m_IntCount;
        e->u.Ih.EntryCount = (ULONGLONG)m_IhEntries;
        e->u.Ih.SdmaTrapCount = (ULONGLONG)m_SdmaTraps;
        e->u.Ih.LastClientId = (ULONG)m_LastClient;
        e->u.Ih.LastSourceId = (ULONG)m_LastSrc;
        e->u.Ih.LastSrcData = (ULONG)m_LastSrcData;
        e->u.Ih.Rptr = m_IhRptr;
        e->u.Ih.Wptr = m_IhReady ? Rreg(BC250_IH_RB_WPTR) : 0;
        e->u.Ih.Overflow = (ULONG)m_IhOverflow;
        break;
    case BC250_CMD_SDMA_START:  R = ToEsc(SdmaStart()); break;
    case BC250_CMD_SDMA_TEST:   R = ToEsc(SdmaTest(&e->u.Test)); break;
    case BC250_CMD_STOP_ALL:
        if (m_SdmaReady) SdmaHalt();
        if (m_IhReady) IhDisable();
        break;
    case BC250_CMD_VRAM_READ:
        if (!m_Scratch) { R = BC250_E_STATE; break; }
        if (e->u.Vram.Count > 64 || (e->u.Vram.Offset & 3) ||
            (ULONGLONG)e->u.Vram.Offset + e->u.Vram.Count * 4ull > BC250_SCRATCH_SIZE) { R = BC250_E_RANGE; break; }
        HdpInvalidate();
        for (ULONG i = 0; i < e->u.Vram.Count; ++i) e->u.Vram.Data[i] = ScrRead(e->u.Vram.Offset + i * 4);
        break;
    case BC250_CMD_SUBMIT_IB:
    {
        ULONG Fence = 0, Us = 0;
        if (e->u.Submit.Count > BC250_IB_MAX_DWORDS) { R = BC250_E_RANGE; break; }
        R = ToEsc(SubmitIb((const ULONG*)e->u.Submit.Ib, e->u.Submit.Count, &Fence, &Us));
        e->u.Submit.Fence = Fence;
        e->u.Submit.ElapsedUs = Us;
        e->u.Submit.IbMc = ScrMc(BC250_SCR_IB);
        e->u.Submit.ScratchMc = m_ScratchMc;
        break;
    }
    case BC250_CMD_FILL_RECT:   R = ToEsc(FillRect(&e->u.Rect)); break;
    case BC250_CMD_CP_START:    R = ToEsc(CpStart()); break;
    case BC250_CMD_CP_TEST:     R = ToEsc(CpTest(&e->u.Test)); break;
    case BC250_CMD_PSP_LOAD:    R = ToEsc(PspLoadAll(&e->u.Test)); break;
    default:
        R = BC250_E_BADCMD;
    }
    ExReleaseFastMutex(&m_Lock);
    e->Header.Status = R;
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------------------------------------
// M5b: services for BC250Kmd (full WDDM driver). PASSIVE_LEVEL unless noted.
// ----------------------------------------------------------------------------------------------

NTSTATUS BC250_GPU::LoadFirmwareFile(PCWSTR FileName, ULONG Engine)
{
    PAGED_CODE();
    if (Engine >= BC250_FW_COUNT) return STATUS_INVALID_PARAMETER;
    WCHAR PathBuf[160];
    NTSTATUS Sp = RtlStringCchPrintfW(PathBuf, ARRAYSIZE(PathBuf), L"%s%s", BC250_FW_DIR, FileName);
    if (!NT_SUCCESS(Sp)) return Sp;
    UNICODE_STRING Name;
    RtlInitUnicodeString(&Name, PathBuf);
    OBJECT_ATTRIBUTES Oa;
    InitializeObjectAttributes(&Oa, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    IO_STATUS_BLOCK Io;
    HANDLE h = NULL;
    NTSTATUS St = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &Oa, &Io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                               FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(St)) return St;
    FILE_STANDARD_INFORMATION Fsi;
    St = ZwQueryInformationFile(h, &Io, &Fsi, sizeof(Fsi), FileStandardInformation);
    if (NT_SUCCESS(St) && (Fsi.EndOfFile.QuadPart < 64 || Fsi.EndOfFile.QuadPart > 1024 * 1024)) St = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS(St))
    {
        ULONG Size = (ULONG)Fsi.EndOfFile.QuadPart;
        UCHAR* Buf = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, BC250_TAG);
        if (!Buf) St = STATUS_INSUFFICIENT_RESOURCES;
        else
        {
            LARGE_INTEGER Off; Off.QuadPart = 0;
            St = ZwReadFile(h, NULL, NULL, NULL, &Io, Buf, Size, &Off, NULL);
            if (NT_SUCCESS(St) && Io.Information == Size)
            {
                UCHAR** pp = (Engine == BC250_FW_SDMA0) ? &m_Fw : &m_FwCp[Engine];
                ULONG*  ps = (Engine == BC250_FW_SDMA0) ? &m_FwSize : &m_FwCpSize[Engine];
                if (*pp) ExFreePoolWithTag(*pp, BC250_TAG);
                *pp = Buf; *ps = Size;
            }
            else { ExFreePoolWithTag(Buf, BC250_TAG); if (NT_SUCCESS(St)) St = STATUS_END_OF_FILE; }
        }
    }
    ZwClose(h);
    return St;
}

ULONGLONG BC250_GPU::VramSegmentSize() const
{
    PAGED_CODE();
    ULONGLONG End = m_BarVram.Length;
    if (m_VramSizeMb && ((ULONGLONG)m_VramSizeMb << 20) < End) End = (ULONGLONG)m_VramSizeMb << 20;
    return End > BC250_KMD_VRAM_OFFSET ? (End - BC250_KMD_VRAM_OFFSET) & ~0xFFFFull : 0;
}

ULONGLONG BC250_GPU::ScanoutMc() const
{
    PAGED_CODE();
    if ((ULONGLONG)m_FbPhys.QuadPart < m_BarVram.PhysAddr) return 0;
    return m_FbStart + ((ULONGLONG)m_FbPhys.QuadPart - m_BarVram.PhysAddr);
}

NTSTATUS BC250_GPU::KmdBringUp()
{
    PAGED_CODE();
    if (!m_Mmio) return STATUS_DEVICE_NOT_READY;
    NTSTATUS St = LoadFirmwareFile(L"cyan_skillfish2_sdma.bin", BC250_FW_SDMA0);   // file I/O at PASSIVE, outside the lock
    if (!NT_SUCCESS(St)) return St;
    // graphics ring is optional: only if all four CP images are present
    BOOLEAN HaveCp = NT_SUCCESS(LoadFirmwareFile(L"cyan_skillfish2_rlc.bin", BC250_FW_RLC)) &&
                     NT_SUCCESS(LoadFirmwareFile(L"cyan_skillfish2_pfp.bin", BC250_FW_PFP)) &&
                     NT_SUCCESS(LoadFirmwareFile(L"cyan_skillfish2_me.bin",  BC250_FW_ME)) &&
                     NT_SUCCESS(LoadFirmwareFile(L"cyan_skillfish2_ce.bin",  BC250_FW_CE));
    ExAcquireFastMutex(&m_Lock);
    St = GartInit();
    if (NT_SUCCESS(St)) St = IhInit();
    if (NT_SUCCESS(St)) { BC250_TEST Psp; St = PspLoadAll(&Psp); }   // also loads CP firmware when present
    if (NT_SUCCESS(St)) St = SdmaStart();
    if (NT_SUCCESS(St) && HaveCp && NT_SUCCESS(CpStart()))
        Rmw(BC250_CP_INT_CNTL_RING0, 0, BC250_CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK);   // EOP interrupts
    if (NT_SUCCESS(St)) { ScrWrite(BC250_SCR_FENCE, 0); ScrWrite(BC250_SCR_CP_FENCE, 0); HdpFlush(); m_KmdMode = TRUE; }
    ExReleaseFastMutex(&m_Lock);
    if (NT_SUCCESS(St) && !VramSegmentSize()) St = STATUS_INSUFFICIENT_RESOURCES;
    return St;
}

VOID BC250_GPU::KmdHalt()
{
    PAGED_CODE();
    if (m_SdmaReady) SdmaHalt();
}

NTSTATUS BC250_GPU::KmdRestart()
{
    PAGED_CODE();
    return SdmaStart();   // reloads microcode (still in memory) and resets the ring
}

BOOLEAN BC250_GPU::KmdWaitIdle(ULONG TimeoutUs)
{
    PAGED_CODE();
    for (ULONG i = 0; i < TimeoutUs; ++i)
    {
        if (Rreg(BC250_SDMA0_GFX_RB_RPTR) == Rreg(BC250_SDMA0_GFX_RB_WPTR) &&
            (Rreg(BC250_SDMA0_STATUS_REG) & BC250_SDMA0_STATUS_REG__IDLE_MASK))
            return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}
