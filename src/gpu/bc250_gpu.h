// SPDX-License-Identifier: MIT
// BC-250 (Cyan Skillfish, gfx1013) hardware access layer for BC250Display.sys.
//
//   M1  MMIO BAR mapping, register/SMN access, SMU mailbox, adapter info
//   M2  IH (interrupt handler) ring and ISR
//   M3  GCVM: system aperture + VMID0 GART page table
//   M4  SDMA0: direct microcode load, GFX ring, WRITE_LINEAR / FENCE / TRAP tests
//
// Everything beyond M1 is opt-in at runtime (bc250ctl + DeveloperMode registry value).
// Nothing here runs at boot except mapping the MMIO BAR and reading registers.
//
// Register sequences follow the Linux amdgpu driver (MIT-licensed files):
// gfxhub_v2_0.c, navi10_ih.c, nbio_v2_3.c, sdma_v5_0.c, smu_cmn.c.
#pragma once

#include "bc250_escape.h"
#include "bc250_regs.h"

// VRAM scratch window owned by the driver (inside the BAR0 aperture, away from the GOP framebuffer).
#define BC250_SCRATCH_OFFSET   0x04000000ull   // 64 MiB into VRAM
#define BC250_SCRATCH_SIZE     0x00800000ull   // 8 MiB

#define BC250_SCR_GART_PT      0x000000u   // 512 KiB = 65536 PTEs -> 256 MiB GART
#define BC250_SCR_IH_RING      0x080000u   // 64 KiB
#define BC250_SCR_IH_WPTR      0x090000u   // IH wptr writeback
#define BC250_SCR_SDMA_RING    0x0A0000u   // 64 KiB
#define BC250_SCR_WB           0x0B0000u   // rptr writeback + test slots
#define BC250_SCR_DUMMY        0x0C0000u   // dummy page for faults
#define BC250_SCR_TEST         0x100000u   // 1 MiB test area
#define BC250_SCR_IB           0x200000u   // 1 MiB indirect buffers (M5)
#define BC250_SCR_FENCE        (BC250_SCR_WB + 0x200u)
#define BC250_SCR_CP_PFP       0x400000u   // 320 KiB each, 4 KiB aligned (M4b)
#define BC250_SCR_CP_CE        0x450000u
#define BC250_SCR_CP_ME        0x4A0000u
#define BC250_SCR_CSB          0x4F0000u   // clear-state buffer
#define BC250_SCR_CP_RING      0x500000u   // 64 KiB graphics ring 0
#define BC250_SCR_CP_WB        0x510000u   // rptr writeback (+0), wptr poll (+8), WRITE_DATA test (+0x100)
#define BC250_CP_RING_SIZE     0x10000u
#define BC250_SCR_CP_FENCE     (BC250_SCR_CP_WB + 0x200u)
#define BC250_IH_CLIENT_GRBM_CP 0x14u
#define BC250_IH_SRC_CP_EOP    181u
#define BC250_FW_DIR           L"\\SystemRoot\\System32\\drivers\\bc250\\"

#define BC250_GART_SIZE        0x10000000ull   // 256 MiB
#define BC250_GART_RESERVED    0x00100000ull   // first 1 MiB of GART kept for the driver (test page)
#define BC250_KMD_VRAM_OFFSET  0x08000000ull   // BC250Kmd VRAM segment starts at 128 MiB
#define BC250_IH_RING_SIZE     0x10000u
#define BC250_SDMA_RING_SIZE   0x10000u

// IH client / source ids (soc15_ih_clientid.h, sdma v5 irqsrcs)
#define BC250_IH_CLIENT_SDMA0  8u
#define BC250_IH_SRC_SDMA_TRAP 224u

class BC250_GPU
{
public:
    BC250_GPU();

    // PASSIVE_LEVEL. Never fails the adapter start: on any problem the GPU layer stays disabled.
    void Start(_In_ const DXGKRNL_INTERFACE* pDxgk, _In_ const DXGK_DEVICE_INFO* pDevInfo,
               _In_ DEVICE_OBJECT* pPdo, _In_ const DXGK_DISPLAY_INFORMATION* pScanout);
    void Stop();     // PASSIVE_LEVEL: halts engines, disables IH, unmaps

    NTSTATUS Escape(_Inout_updates_bytes_(Size) VOID* pData, _In_ UINT Size);   // PASSIVE_LEVEL
    BOOLEAN  Isr();  // DIRQL

    // --- M5b: services for the full WDDM driver (BC250Kmd) ---
    NTSTATUS  KmdBringUp();                      // PASSIVE: GART + IH + SDMA (firmware from disk)
    VOID      KmdHalt();                         // PASSIVE: TDR reset
    NTSTATUS  KmdRestart();                      // PASSIVE: TDR restart
    ULONGLONG VramSegmentMc() const   { return m_FbStart + BC250_KMD_VRAM_OFFSET; }
    ULONGLONG VramSegmentCpu() const  { return m_BarVram.PhysAddr + BC250_KMD_VRAM_OFFSET; }
    ULONGLONG VramSegmentSize() const;
    ULONGLONG ApertureBase() const    { return m_GartStart + BC250_GART_RESERVED; }
    ULONGLONG ApertureSize() const    { return BC250_GART_SIZE - BC250_GART_RESERVED; }
    ULONGLONG ScanoutMc() const;
    ULONG     ScanoutPitch() const    { return m_FbPitch; }
    VOID      GartMapPages(ULONGLONG GpuAddr, _In_reads_(Count) const PFN_NUMBER* Pfns, SIZE_T Count);  // <= DISPATCH
    VOID      GartUnmapPages(ULONGLONG GpuAddr, SIZE_T Count);                                         // <= DISPATCH
    VOID      KmdSubmit(ULONGLONG IbMc, ULONG Dwords, ULONG FenceId);   // node 0 (SDMA); any IRQL, dxgkrnl serializes
    VOID      KmdSubmitCp(ULONGLONG IbMc, ULONG Dwords, ULONG FenceId); // node 1 (CP graphics ring)
    ULONG     KmdCompletedFence() const { return m_Scratch ? ScrRead(BC250_SCR_FENCE) : 0; }
    ULONG     KmdCompletedFenceCp() const { return m_Scratch ? ScrRead(BC250_SCR_CP_FENCE) : 0; }
    BOOLEAN   KmdCpReady() const { return m_CpReady; }
    BOOLEAN   KmdWaitIdle(ULONG TimeoutUs);      // PASSIVE

private:
    // --- M1 ---
    ULONG    Rreg(ULONG Dw) const;
    VOID     Wreg(ULONG Dw, ULONG Value);
    VOID     Rmw(ULONG Dw, ULONG Clear, ULONG Set) { Wreg(Dw, (Rreg(Dw) & ~Clear) | Set); }
    NTSTATUS SmuSend(ULONG Msg, ULONG Arg, _Out_ bc_u32* pResp, _Out_ bc_u32* pOut);
    VOID     HdpFlush();
    VOID     HdpInvalidate();
    VOID     FillInfo(_Out_ BC250_INFO* pInfo);
    VOID     ReadMemoryLayout();
    BOOLEAN  ReadDeveloperMode();
    NTSTATUS MapScratch();

    ULONG    ScrRead(ULONG Off) const  { return READ_REGISTER_ULONG((PULONG)(m_Scratch + Off)); }
    VOID     ScrWrite(ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((PULONG)(m_Scratch + Off), V); }
    ULONGLONG ScrMc(ULONG Off) const   { return m_ScratchMc + Off; }
    ULONGLONG McToPhys(ULONGLONG Mc) const { return m_VramBasePhys + (Mc - m_FbStart); }

    // --- M2 ---
    NTSTATUS IhInit();
    VOID     IhDisable();

    // --- M3 ---
    NTSTATUS GartInit();
    NTSTATUS GartMapTestPage();
    NTSTATUS TlbFlush();

    // --- M4 ---
    NTSTATUS FwUpload(_In_ const BC250_FW* pFw);
    // --- M4b: RLC + CP graphics ---
    NTSTATUS CpStart();
    NTSTATUS CpLoadIc(ULONG Engine, ULONG ScrOff, ULONG OpCntl, ULONG BaseCntl, ULONG BaseLo, ULONG BaseHi, ULONG UcodeAddr, ULONG UcodeData);
    VOID     CpWrite(ULONG Dw);
    VOID     CpCommit();
    NTSTATUS CpTest(_Out_ BC250_TEST* pTest);
    NTSTATUS SdmaStart();
    VOID     SdmaHalt();
    VOID     SdmaWrite(ULONG Dw);
    VOID     SdmaCommit();
    NTSTATUS SdmaTest(_Out_ BC250_TEST* pTest);
    // --- M5: command submission ---
    NTSTATUS SubmitIb(_In_reads_(Count) const ULONG* pIb, ULONG Count, _Out_ ULONG* pFence, _Out_ ULONG* pUs);
    NTSTATUS FillRect(_Inout_ BC250_RECT* pRect);

    BOOLEAN  WaitScratch(ULONG Off, ULONG Expect, ULONG TimeoutUs, _Out_ bc_u32* pSeen);

    // state
    DXGKRNL_INTERFACE m_Dxgk;
    DEVICE_OBJECT*    m_pPdo;
    BOOLEAN           m_DeveloperMode;
    FAST_MUTEX        m_Lock;           // serializes escapes, SMU and SMN access

    ULONG m_VendorId, m_DeviceId, m_SubsysId, m_Revision;
    BC250_BAR m_BarVram, m_BarDoorbell, m_BarMmio;
    volatile UCHAR* m_Mmio;

    PHYSICAL_ADDRESS m_FbPhys;          // GOP framebuffer (scanout)
    ULONG            m_FbSize;
    ULONG            m_FbWidth, m_FbHeight, m_FbPitch;
    ULONG            m_SdmaFence;       // last fence value emitted

    ULONGLONG m_FbStart, m_FbEnd, m_VramBasePhys;
    ULONG     m_VramSizeMb;

    volatile UCHAR* m_Scratch;          // CPU mapping of the VRAM scratch window
    ULONGLONG       m_ScratchMc;

    BOOLEAN   m_GartReady, m_IhReady, m_SdmaReady;
    ULONGLONG m_GartStart, m_GartEnd;
    PVOID            m_TestPage;        // system page mapped at GART offset 0
    PHYSICAL_ADDRESS m_TestPagePhys;

    // IH (touched at DIRQL)
    ULONG              m_IhRptr;
    volatile LONG64    m_IntCount, m_IhEntries, m_SdmaTraps;
    volatile LONG      m_LastClient, m_LastSrc, m_LastSrcData, m_IhOverflow;

    BOOLEAN   m_KmdMode;                // full WDDM driver: notify dxgkrnl from the ISR
    NTSTATUS  LoadFirmwareFile(_In_ PCWSTR Name, ULONG Engine);   // file name under BC250_FW_DIR

    // SDMA
    UCHAR*    m_Fw;                     // SDMA0
    ULONG     m_FwSize;
    UCHAR*    m_FwCp[BC250_FW_COUNT];   // RLC, PFP, ME, CE (index = BC250_FW_*)
    ULONG     m_FwCpSize[BC250_FW_COUNT];
    BOOLEAN   m_CpReady;
    ULONG     m_CpWptr;                 // dwords
    ULONG     m_SdmaWptr;               // in dwords
};
