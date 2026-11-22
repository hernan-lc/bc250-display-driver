// SPDX-License-Identifier: MIT
// M4b: RLC + CP graphics bring-up for the BC-250 (gfx_v10_0.c, AMDGPU_FW_LOAD_DIRECT path).
// Golden registers and the clear-state buffer are generated into bc250_regs.h by tools/genregs.py.

#include "..\bdd.hxx"

#pragma code_seg("PAGE")

#define PKT3(op, n) ((3u << 30) | (((op) & 0xFFu) << 8) | (((n) & 0x3FFFu) << 16))
#define PKT3_SET_BASE        0x11
#define PKT3_WRITE_DATA      0x37
#define PKT3_SET_UCONFIG_REG 0x79
#define RLCG_UCODE_LOADING_START_ADDRESS 0x2000u

static ULONG CpFieldShift(ULONG Mask) { ULONG s = 0; while (Mask && !(Mask & 1)) { Mask >>= 1; ++s; } return s; }
#define CP_SETF(v, mask, val) (((v) & ~(mask)) | ((((ULONG)(val)) << CpFieldShift(mask)) & (mask)))
static ULONG CpLog2(ULONG v) { ULONG r = 0; while (v > 1) { v >>= 1; ++r; } return r; }

static const ULONG g_GoldenGc[][3] = BC250_GOLDEN_GC_INIT;
static const ULONG g_Csb[] = BC250_CSB_INIT;

#pragma code_seg(push)
#pragma code_seg()   // submission path: callable at raised IRQL
VOID BC250_GPU::CpWrite(ULONG Dw)
{
    ScrWrite(BC250_SCR_CP_RING + (m_CpWptr % (BC250_CP_RING_SIZE / 4)) * 4, Dw);
    ++m_CpWptr;
}

VOID BC250_GPU::CpCommit()
{
    HdpFlush();
    Wreg(BC250_CP_RB0_WPTR, m_CpWptr);       // the gfx ring wptr is in dwords
    Wreg(BC250_CP_RB0_WPTR_HI, 0);
}

// PM4: INDIRECT_BUFFER + RELEASE_MEM(fence, EOP interrupt) - gfx_v10_0_ring_emit_ib_gfx / emit_fence
VOID BC250_GPU::KmdSubmitCp(ULONGLONG IbMc, ULONG Dwords, ULONG FenceId)
{
    if (!m_CpReady) return;
    if (Dwords)
    {
        CpWrite(PKT3(0x3F, 2));                   // INDIRECT_BUFFER
        CpWrite((ULONG)IbMc & 0xFFFFFFFC);
        CpWrite((ULONG)(IbMc >> 32));
        CpWrite(Dwords & 0xFFFFF);                // VMID 0
    }
    ULONGLONG F = ScrMc(BC250_SCR_CP_FENCE);
    CpWrite(PKT3(0x49, 6));                       // RELEASE_MEM
    CpWrite((1u << 22) | (1u << 21) | (1u << 13) | (1u << 12) | (3u << 25) | 0x14u | (5u << 8));  // GCR_SEQ|GL2_WB|GLM_INV|GLM_WB|BYPASS|CACHE_FLUSH_AND_INV_TS_EVENT|INDEX5
    CpWrite((1u << 29) | (2u << 24));             // DATA_SEL=32-bit, INT_SEL=after write confirm
    CpWrite((ULONG)F);
    CpWrite((ULONG)(F >> 32));
    CpWrite(FenceId);
    CpWrite(0);
    CpWrite(0);
    CpCommit();
}
#pragma code_seg(pop)

// Copies a gfx_firmware_header_v1_0 image into VRAM, points the engine's instruction cache at it
// and loads the jump table through the HYP ucode port (gfx_v10_0_cp_gfx_load_{pfp,ce,me}_microcode).
NTSTATUS BC250_GPU::CpLoadIc(ULONG Engine, ULONG ScrOff, ULONG OpCntl, ULONG BaseCntl, ULONG BaseLo, ULONG BaseHi,
                             ULONG UcodeAddr, ULONG UcodeData)
{
    PAGED_CODE();
    const UCHAR* Fw = m_FwCp[Engine];
    ULONG FwSize = m_FwCpSize[Engine];
    if (!Fw || FwSize < 44) return STATUS_INVALID_DEVICE_STATE;
    ULONG Version = *(const ULONG*)(Fw + 16), Bytes = *(const ULONG*)(Fw + 20), Off = *(const ULONG*)(Fw + 24);
    ULONG JtOff = *(const ULONG*)(Fw + 36), JtSize = *(const ULONG*)(Fw + 40);
    if (Off >= FwSize || Bytes > FwSize - Off || Bytes > 0x50000 || (Bytes & 3)) return STATUS_INVALID_IMAGE_FORMAT;
    if ((ULONGLONG)(JtOff + JtSize) * 4 > Bytes) return STATUS_INVALID_IMAGE_FORMAT;
    const ULONG* Code = (const ULONG*)(Fw + Off);

    for (ULONG i = 0; i < Bytes / 4; ++i) ScrWrite(ScrOff + i * 4, Code[i]);
    HdpFlush();

    // invalidate the L1 instruction cache (same bit layout for PFP, CE and ME)
    Rmw(OpCntl, 0, BC250_CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE_MASK);
    ULONG i;
    for (i = 0; i < 50000; ++i)
    {
        if (Rreg(OpCntl) & BC250_CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK) break;
        KeStallExecutionProcessor(1);
    }
    if (i >= 50000) return STATUS_IO_TIMEOUT;

    ULONG t = Rreg(BaseCntl);
    t &= ~(BC250_CP_PFP_IC_BASE_CNTL__VMID_MASK | BC250_CP_PFP_IC_BASE_CNTL__CACHE_POLICY_MASK |
           BC250_CP_PFP_IC_BASE_CNTL__EXE_DISABLE_MASK);
    t |= BC250_CP_PFP_IC_BASE_CNTL__ADDRESS_CLAMP_MASK;
    Wreg(BaseCntl, t);
    ULONGLONG Mc = ScrMc(ScrOff);
    Wreg(BaseLo, (ULONG)Mc & 0xFFFFF000);
    Wreg(BaseHi, (ULONG)(Mc >> 32));

    Wreg(UcodeAddr, 0);
    for (ULONG j = 0; j < JtSize; ++j) Wreg(UcodeData, Code[JtOff + j]);
    Wreg(UcodeAddr, Version);
    return STATUS_SUCCESS;
}

NTSTATUS BC250_GPU::CpStart()
{
    PAGED_CODE();
    if (!m_GartReady) return STATUS_INVALID_DEVICE_STATE;   // CP fetches through GCVM, like SDMA
    for (ULONG e = BC250_FW_RLC; e <= BC250_FW_CE; ++e) if (!m_FwCp[e]) return STATUS_INVALID_DEVICE_STATE;

    // golden registers (soc15_program_register_sequence)
    for (ULONG i = 0; i < ARRAYSIZE(g_GoldenGc); ++i)
    {
        if (g_GoldenGc[i][1] == 0xFFFFFFFF) Wreg(g_GoldenGc[i][0], g_GoldenGc[i][2]);
        else Rmw(g_GoldenGc[i][0], g_GoldenGc[i][1], g_GoldenGc[i][2] & g_GoldenGc[i][1]);
    }

    const BOOLEAN ViaPsp = (m_PspLoaded & (1u << BC250_FW_RLC)) != 0;   // PSP non-autoload path
    // ---- RLC (gfx_v10_0_rlc_resume) ----
    Rmw(BC250_RLC_CNTL, BC250_RLC_CNTL__RLC_ENABLE_F32_MASK, 0);        // stop
    Wreg(BC250_RLC_CGCG_CGLS_CTRL, 0);                                  // no clock gating
    Wreg(BC250_RLC_PG_CNTL, 0);                                         // no power gating
    if (!ViaPsp)
    {
        const UCHAR* Fw = m_FwCp[BC250_FW_RLC];
        ULONG Version = *(const ULONG*)(Fw + 16), Bytes = *(const ULONG*)(Fw + 20), Off = *(const ULONG*)(Fw + 24);
        if (Off >= m_FwCpSize[BC250_FW_RLC] || Bytes > m_FwCpSize[BC250_FW_RLC] - Off || (Bytes & 3))
            return STATUS_INVALID_IMAGE_FORMAT;
        const ULONG* Code = (const ULONG*)(Fw + Off);
        Wreg(BC250_RLC_GPM_UCODE_ADDR, RLCG_UCODE_LOADING_START_ADDRESS);
        for (ULONG i = 0; i < Bytes / 4; ++i) Wreg(BC250_RLC_GPM_UCODE_DATA, Code[i]);
        Wreg(BC250_RLC_GPM_UCODE_ADDR, Version);
    }
    // clear-state buffer
    for (ULONG i = 0; i < BC250_CSB_DWORDS; ++i) ScrWrite(BC250_SCR_CSB + i * 4, g_Csb[i]);
    HdpFlush();
    ULONGLONG Csb = ScrMc(BC250_SCR_CSB);
    Wreg(BC250_RLC_CSIB_ADDR_HI, (ULONG)(Csb >> 32));
    Wreg(BC250_RLC_CSIB_ADDR_LO, (ULONG)Csb & 0xFFFFFFFC);
    Wreg(BC250_RLC_CSIB_LENGTH, BC250_CSB_DWORDS);
    // start; RLC_PG_CNTL[23]=1: no RLC<->SMU handshake, so GFXOFF stays disabled
    Rmw(BC250_RLC_PG_CNTL, 0, 0x800000);
    Rmw(BC250_RLC_CNTL, 0, BC250_RLC_CNTL__RLC_ENABLE_F32_MASK);
    KeStallExecutionProcessor(50);

    // ---- CP gfx microcode: halt, then PFP/CE/ME through the instruction caches ----
    const ULONG Halt = BC250_CP_ME_CNTL__ME_HALT_MASK | BC250_CP_ME_CNTL__PFP_HALT_MASK | BC250_CP_ME_CNTL__CE_HALT_MASK;
    Rmw(BC250_CP_ME_CNTL, 0, Halt);
    for (ULONG i = 0; i < 100000 && Rreg(BC250_CP_STAT) != 0; ++i) KeStallExecutionProcessor(1);
    NTSTATUS St = STATUS_SUCCESS;
    if (!ViaPsp) St = CpLoadIc(BC250_FW_PFP, BC250_SCR_CP_PFP, BC250_CP_PFP_IC_OP_CNTL, BC250_CP_PFP_IC_BASE_CNTL, BC250_CP_PFP_IC_BASE_LO,
                  BC250_CP_PFP_IC_BASE_HI, BC250_CP_HYP_PFP_UCODE_ADDR, BC250_CP_HYP_PFP_UCODE_DATA);
    if (!NT_SUCCESS(St)) return St;
    if (!ViaPsp) St = CpLoadIc(BC250_FW_CE, BC250_SCR_CP_CE, BC250_CP_CE_IC_OP_CNTL, BC250_CP_CE_IC_BASE_CNTL, BC250_CP_CE_IC_BASE_LO,
                  BC250_CP_CE_IC_BASE_HI, BC250_CP_HYP_CE_UCODE_ADDR, BC250_CP_HYP_CE_UCODE_DATA);
    if (!NT_SUCCESS(St)) return St;
    if (!ViaPsp) St = CpLoadIc(BC250_FW_ME, BC250_SCR_CP_ME, BC250_CP_ME_IC_OP_CNTL, BC250_CP_ME_IC_BASE_CNTL, BC250_CP_ME_IC_BASE_LO,
                  BC250_CP_ME_IC_BASE_HI, BC250_CP_HYP_ME_UCODE_ADDR, BC250_CP_HYP_ME_UCODE_DATA);
    if (!NT_SUCCESS(St)) return St;

    // ---- graphics ring 0 (gfx_v10_0_cp_gfx_resume) ----
    for (ULONG o = 0; o < BC250_CP_RING_SIZE; o += 4) ScrWrite(BC250_SCR_CP_RING + o, 0);
    for (ULONG o = 0; o < 0x1000; o += 4) ScrWrite(BC250_SCR_CP_WB + o, 0);
    HdpFlush();
    Wreg(BC250_CP_RB_WPTR_DELAY, 0);
    Wreg(BC250_CP_RB_VMID, 0);
    Rmw(BC250_GRBM_GFX_CNTL, BC250_GRBM_GFX_CNTL__PIPEID_MASK, 0);
    ULONG BufSz = CpLog2(BC250_CP_RING_SIZE / 8);
    ULONG Cntl = CP_SETF(0, BC250_CP_RB0_CNTL__RB_BUFSZ_MASK, BufSz);
    Cntl = CP_SETF(Cntl, BC250_CP_RB0_CNTL__RB_BLKSZ_MASK, BufSz - 2);
    Wreg(BC250_CP_RB0_CNTL, Cntl);
    m_CpWptr = 0;
    Wreg(BC250_CP_RB0_WPTR, 0);
    Wreg(BC250_CP_RB0_WPTR_HI, 0);
    ULONGLONG Rptr = ScrMc(BC250_SCR_CP_WB), WptrPoll = ScrMc(BC250_SCR_CP_WB + 8);
    Wreg(BC250_CP_RB0_RPTR_ADDR, (ULONG)Rptr);
    Wreg(BC250_CP_RB0_RPTR_ADDR_HI, (ULONG)(Rptr >> 32) & 0xFFFF);
    Wreg(BC250_CP_RB_WPTR_POLL_ADDR_LO, (ULONG)WptrPoll);
    Wreg(BC250_CP_RB_WPTR_POLL_ADDR_HI, (ULONG)(WptrPoll >> 32));
    KeStallExecutionProcessor(1000);
    Wreg(BC250_CP_RB0_CNTL, Cntl);
    ULONGLONG Ring = ScrMc(BC250_SCR_CP_RING) >> 8;
    Wreg(BC250_CP_RB0_BASE, (ULONG)Ring);
    Wreg(BC250_CP_RB0_BASE_HI, (ULONG)(Ring >> 32));
    Wreg(BC250_CP_RB_ACTIVE, 1);
    Rmw(BC250_CP_RB_DOORBELL_CONTROL, BC250_CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK, 0);

    // ---- start (gfx_v10_0_cp_gfx_start) ----
    Wreg(BC250_CP_MAX_CONTEXT, 7);
    Wreg(BC250_CP_DEVICE_ID, 1);
    Rmw(BC250_CP_ME_CNTL, Halt, 0);
    for (ULONG i = 0; i < 100000 && Rreg(BC250_CP_STAT) != 0; ++i) KeStallExecutionProcessor(1);
    for (ULONG i = 0; i < BC250_CSB_DWORDS; ++i) CpWrite(g_Csb[i]);   // preamble, context state, clear state
    CpWrite(PKT3(PKT3_SET_BASE, 2));
    CpWrite(3);                                                       // CE_PARTITION_BASE
    CpWrite(0x8000);
    CpWrite(0x8000);
    CpCommit();
    m_CpReady = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS BC250_GPU::CpTest(BC250_TEST* t)
{
    PAGED_CODE();
    RtlZeroMemory(t, sizeof(*t));
    if (!m_CpReady) return STATUS_INVALID_DEVICE_STATE;
    LARGE_INTEGER T0 = KeQueryPerformanceCounter(NULL), Freq;
    BOOLEAN Ok = FALSE;

    // 1) gfx_v10_0_ring_test_ring: SET_UCONFIG_REG(SCRATCH_REG0)
    t->Step = 1;
    Wreg(BC250_SCRATCH_REG0, 0xCAFEDEAD);
    CpWrite(PKT3(PKT3_SET_UCONFIG_REG, 1));
    CpWrite(BC250_SCRATCH_REG0 - 0xC000);
    CpWrite(0xDEADBEEF);
    CpCommit();
    t->Expected = 0xDEADBEEF;
    for (ULONG i = 0; i < 100000; ++i)
    {
        t->Observed = Rreg(BC250_SCRATCH_REG0);
        if (t->Observed == 0xDEADBEEF) { Ok = TRUE; break; }
        KeStallExecutionProcessor(1);
    }
    if (Ok)
    {
        // 2) WRITE_DATA to memory: DST_SEL=5 (memory), WR_CONFIRM, ENGINE_SEL=ME
        t->Step = 2;
        const ULONG Slot = BC250_SCR_CP_WB + 0x100;
        ScrWrite(Slot, 0);
        HdpFlush();
        ULONGLONG Dst = ScrMc(Slot);
        CpWrite(PKT3(PKT3_WRITE_DATA, 3));
        CpWrite((5u << 8) | (1u << 20));
        CpWrite((ULONG)Dst);
        CpWrite((ULONG)(Dst >> 32));
        CpWrite(0x00C0FFEE);
        CpCommit();
        t->Expected = 0x00C0FFEE;
        bc_u32 Seen = 0;
        Ok = WaitScratch(Slot, 0x00C0FFEE, 100000, &Seen);
        t->Observed = Seen;
        if (Ok) t->Step = 100;
    }
    LARGE_INTEGER T1 = KeQueryPerformanceCounter(&Freq);
    t->ElapsedUs = (ULONG)(((T1.QuadPart - T0.QuadPart) * 1000000) / (Freq.QuadPart ? Freq.QuadPart : 1));
    t->Rptr = ScrRead(BC250_SCR_CP_WB);
    t->Wptr = m_CpWptr;
    t->SdmaStatus = Rreg(BC250_CP_STAT);            // reused field: CP_STAT
    t->FaultStatus = Rreg(BC250_GCVM_L2_PROTECTION_FAULT_STATUS);
    return Ok ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}
