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
#define BC250_SCRATCH_SIZE     0x00400000ull   // 4 MiB

#define BC250_SCR_GART_PT      0x000000u   // 512 KiB = 65536 PTEs -> 256 MiB GART
#define BC250_SCR_IH_RING      0x080000u   // 64 KiB
#define BC250_SCR_IH_WPTR      0x090000u   // IH wptr writeback
#define BC250_SCR_SDMA_RING    0x0A0000u   // 64 KiB
#define BC250_SCR_WB           0x0B0000u   // rptr writeback + test slots
#define BC250_SCR_DUMMY        0x0C0000u   // dummy page for faults
#define BC250_SCR_TEST         0x100000u   // 1 MiB test area

#define BC250_GART_SIZE        0x10000000ull   // 256 MiB
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
               _In_ DEVICE_OBJECT* pPdo, _In_ PHYSICAL_ADDRESS FramebufferPhys, _In_ ULONG FramebufferSize);
    void Stop();     // PASSIVE_LEVEL: halts engines, disables IH, unmaps

    NTSTATUS Escape(_Inout_updates_bytes_(Size) VOID* pData, _In_ UINT Size);   // PASSIVE_LEVEL
    BOOLEAN  Isr();  // DIRQL

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
    NTSTATUS SdmaStart();
    VOID     SdmaHalt();
    VOID     SdmaWrite(ULONG Dw);
    VOID     SdmaCommit();
    NTSTATUS SdmaTest(_Out_ BC250_TEST* pTest);
    BOOLEAN  WaitScratch(ULONG Off, ULONG Expect, ULONG TimeoutUs, _Out_ bc_u32* pSeen);

    // state
    DXGKRNL_INTERFACE m_Dxgk;
    DEVICE_OBJECT*    m_pPdo;
    BOOLEAN           m_DeveloperMode;
    FAST_MUTEX        m_Lock;           // serializes escapes, SMU and SMN access

    ULONG m_VendorId, m_DeviceId, m_SubsysId, m_Revision;
    BC250_BAR m_BarVram, m_BarDoorbell, m_BarMmio;
    volatile UCHAR* m_Mmio;

    PHYSICAL_ADDRESS m_FbPhys;          // GOP framebuffer
    ULONG            m_FbSize;

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

    // SDMA
    UCHAR*    m_Fw;
    ULONG     m_FwSize;
    ULONG     m_SdmaWptr;               // in dwords
};
