// SPDX-License-Identifier: MIT
// M4d: MEC compute queue on the BC-250. ME1 / pipe 0 / queue 0 is programmed directly through the
// HQD registers, the way amdgpu programs its KIQ (gfx_v10_0_kiq_init_register + compute_mqd_init).
// No doorbells: the MEC polls the write pointer from memory (CP_PQ_WPTR_POLL_CNTL.EN).
// MEC microcode is loaded by the PSP (bc250_gpu_psp.cxx): main image (CP_MEC) + jump table (CP_MEC_ME1).

#include "..\bdd.hxx"

#pragma code_seg("PAGE")

#define PKT3(op, n) ((3u << 30) | (((op) & 0xFFu) << 8) | (((n) & 0x3FFFu) << 16))
#define PKT3_WRITE_DATA  0x37
#define PKT3_RELEASE_MEM 0x49
#define GFX10_MEC_HPD_SIZE 2048u

static ULONG MecShift(ULONG Mask) { ULONG s = 0; while (Mask && !(Mask & 1)) { Mask >>= 1; ++s; } return s; }
#define MEC_SETF(v, mask, val) (((v) & ~(mask)) | ((((ULONG)(val)) << MecShift(mask)) & (mask)))
static ULONG MecLog2(ULONG v) { ULONG r = 0; while (v > 1) { v >>= 1; ++r; } return r; }

VOID BC250_GPU::GrbmSelect(ULONG Me, ULONG Pipe, ULONG Queue)
{
    PAGED_CODE();
    ULONG v = 0;
    v = MEC_SETF(v, BC250_GRBM_GFX_CNTL__PIPEID_MASK, Pipe);
    v = MEC_SETF(v, BC250_GRBM_GFX_CNTL__MEID_MASK, Me);
    v = MEC_SETF(v, BC250_GRBM_GFX_CNTL__QUEUEID_MASK, Queue);
    Wreg(BC250_GRBM_GFX_CNTL, v);
}

#pragma code_seg(push)
#pragma code_seg()   // submission path
VOID BC250_GPU::MecWrite(ULONG Dw)
{
    ScrWrite(BC250_SCR_MEC_RING + (m_MecWptr % (BC250_MEC_RING_SIZE / 4)) * 4, Dw);
    ++m_MecWptr;
}

VOID BC250_GPU::MecCommit()
{
    // the MEC polls the wptr shadow (64-bit, dwords) at BC250_SCR_MEC_WB + 8
    ScrWrite(BC250_SCR_MEC_WB + 8, m_MecWptr);
    ScrWrite(BC250_SCR_MEC_WB + 12, 0);
    HdpFlush();
}
#pragma code_seg(pop)

NTSTATUS BC250_GPU::MecStart()
{
    PAGED_CODE();
    if (!(m_PspLoaded & (1u << BC250_FW_MEC))) return STATUS_INVALID_DEVICE_STATE;
    m_MecReady = FALSE;

    // unhalt MEC1/MEC2 (gfx_v10_0_cp_compute_enable)
    Wreg(BC250_CP_MEC_CNTL, 0);
    KeStallExecutionProcessor(50);

    // memory: MQD (4 KiB), EOP (2 KiB), ring (64 KiB), writeback
    for (ULONG o = 0; o < 0x1000; o += 4) ScrWrite(BC250_SCR_MEC_MQD + o, 0);
    for (ULONG o = 0; o < 0x1000; o += 4) ScrWrite(BC250_SCR_MEC_EOP + o, 0);
    for (ULONG o = 0; o < BC250_MEC_RING_SIZE; o += 4) ScrWrite(BC250_SCR_MEC_RING + o, 0);
    for (ULONG o = 0; o < 0x1000; o += 4) ScrWrite(BC250_SCR_MEC_WB + o, 0);
    ScrWrite(BC250_SCR_MEC_MQD + 0, 0xC0310800);    // v10_compute_mqd header
    HdpFlush();
    m_MecWptr = 0;

    ULONGLONG Mqd = ScrMc(BC250_SCR_MEC_MQD), Eop = ScrMc(BC250_SCR_MEC_EOP) >> 8;
    ULONGLONG Ring = ScrMc(BC250_SCR_MEC_RING) >> 8;
    ULONGLONG Rptr = ScrMc(BC250_SCR_MEC_WB), Wptr = ScrMc(BC250_SCR_MEC_WB + 8);

    GrbmSelect(1, 0, 0);

    // disable the queue if active
    if (Rreg(BC250_CP_HQD_ACTIVE) & 1)
    {
        Wreg(BC250_CP_HQD_DEQUEUE_REQUEST, 1);
        for (ULONG i = 0; i < 100000 && (Rreg(BC250_CP_HQD_ACTIVE) & 1); ++i) KeStallExecutionProcessor(1);
        Wreg(BC250_CP_HQD_DEQUEUE_REQUEST, 0);
        Wreg(BC250_CP_HQD_PQ_RPTR, 0);
    }
    Wreg(BC250_CP_HQD_PQ_DOORBELL_CONTROL, 0);
    Wreg(BC250_CP_HQD_EOP_BASE_ADDR, (ULONG)Eop);
    Wreg(BC250_CP_HQD_EOP_BASE_ADDR_HI, (ULONG)(Eop >> 32));
    Wreg(BC250_CP_HQD_EOP_CONTROL, MEC_SETF(Rreg(BC250_CP_HQD_EOP_CONTROL), BC250_CP_HQD_EOP_CONTROL__EOP_SIZE_MASK,
                                            MecLog2(GFX10_MEC_HPD_SIZE / 4) - 1));
    Wreg(BC250_CP_MQD_BASE_ADDR, (ULONG)Mqd & 0xFFFFFFFC);
    Wreg(BC250_CP_MQD_BASE_ADDR_HI, (ULONG)(Mqd >> 32));
    Wreg(BC250_CP_MQD_CONTROL, Rreg(BC250_CP_MQD_CONTROL) & ~0xFu);                  // VMID 0
    Wreg(BC250_CP_HQD_PQ_BASE, (ULONG)Ring);
    Wreg(BC250_CP_HQD_PQ_BASE_HI, (ULONG)(Ring >> 32));
    ULONG Pq = Rreg(BC250_CP_HQD_PQ_CONTROL);
    Pq = MEC_SETF(Pq, BC250_CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK, MecLog2(BC250_MEC_RING_SIZE / 4) - 1);
    Pq = MEC_SETF(Pq, BC250_CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE_MASK, MecLog2(4096 / 4) - 1);
    Pq |= BC250_CP_HQD_PQ_CONTROL__UNORD_DISPATCH_MASK | BC250_CP_HQD_PQ_CONTROL__PRIV_STATE_MASK |
          BC250_CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK;
    Wreg(BC250_CP_HQD_PQ_CONTROL, Pq);
    Wreg(BC250_CP_HQD_PQ_RPTR_REPORT_ADDR, (ULONG)Rptr & 0xFFFFFFFC);
    Wreg(BC250_CP_HQD_PQ_RPTR_REPORT_ADDR_HI, (ULONG)(Rptr >> 32) & 0xFFFF);
    Wreg(BC250_CP_HQD_PQ_WPTR_POLL_ADDR, (ULONG)Wptr & 0xFFFFFFFC);
    Wreg(BC250_CP_HQD_PQ_WPTR_POLL_ADDR_HI, (ULONG)(Wptr >> 32) & 0xFFFF);
    Wreg(BC250_CP_HQD_PQ_WPTR_LO, 0);
    Wreg(BC250_CP_HQD_PQ_WPTR_HI, 0);
    Wreg(BC250_CP_HQD_VMID, 0);
    Wreg(BC250_CP_HQD_PERSISTENT_STATE, MEC_SETF(Rreg(BC250_CP_HQD_PERSISTENT_STATE),
                                                 BC250_CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE_MASK, 0x53));
    Wreg(BC250_CP_HQD_IB_CONTROL, MEC_SETF(Rreg(BC250_CP_HQD_IB_CONTROL), BC250_CP_HQD_IB_CONTROL__MIN_IB_AVAIL_SIZE_MASK, 3));
    ULONG Qt = MEC_SETF(0, BC250_CP_HQD_QUANTUM__QUANTUM_EN_MASK, 1);
    Qt = MEC_SETF(Qt, BC250_CP_HQD_QUANTUM__QUANTUM_SCALE_MASK, 1);
    Qt = MEC_SETF(Qt, BC250_CP_HQD_QUANTUM__QUANTUM_DURATION_MASK, 1);
    Wreg(BC250_CP_HQD_QUANTUM, Qt);
    Wreg(BC250_CP_HQD_ACTIVE, 1);

    GrbmSelect(0, 0, 0);

    // no doorbells: let the MEC poll write pointers from memory
    Rmw(BC250_CP_PQ_WPTR_POLL_CNTL, 0, BC250_CP_PQ_WPTR_POLL_CNTL__EN_MASK);
    m_MecReady = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS BC250_GPU::MecTest(BC250_TEST* t)
{
    PAGED_CODE();
    RtlZeroMemory(t, sizeof(*t));
    if (!m_MecReady) return STATUS_INVALID_DEVICE_STATE;
    LARGE_INTEGER T0 = KeQueryPerformanceCounter(NULL), Freq;
    bc_u32 Seen = 0;
    BOOLEAN Ok;

    // 1) WRITE_DATA to memory
    t->Step = 1;
    const ULONG Slot = BC250_SCR_MEC_WB + 0x100;
    ScrWrite(Slot, 0);
    HdpFlush();
    ULONGLONG Dst = ScrMc(Slot);
    MecWrite(PKT3(PKT3_WRITE_DATA, 3));
    MecWrite((5u << 8) | (1u << 20));             // DST_SEL memory, WR_CONFIRM
    MecWrite((ULONG)Dst);
    MecWrite((ULONG)(Dst >> 32));
    MecWrite(0xC0FFEE02);
    MecCommit();
    t->Expected = 0xC0FFEE02;
    Ok = WaitScratch(Slot, 0xC0FFEE02, 200000, &Seen);
    t->Observed = Seen;

    // 2) RELEASE_MEM fence (end-of-pipe write, what kernel fences use)
    if (Ok)
    {
        t->Step = 2;
        const ULONG Fence = BC250_SCR_MEC_WB + 0x200;
        ScrWrite(Fence, 0);
        HdpFlush();
        ULONGLONG F = ScrMc(Fence);
        MecWrite(PKT3(PKT3_RELEASE_MEM, 6));
        MecWrite((1u << 22) | (1u << 21) | (1u << 13) | (1u << 12) | (3u << 25) | 0x14u | (5u << 8));
        MecWrite(1u << 29);                       // DATA_SEL 32-bit, no interrupt
        MecWrite((ULONG)F);
        MecWrite((ULONG)(F >> 32));
        MecWrite(0x00000250);
        MecWrite(0);
        MecWrite(0);
        MecCommit();
        t->Expected = 0x250;
        Ok = WaitScratch(Fence, 0x250, 200000, &Seen);
        t->Observed = Seen;
        if (Ok) t->Step = 100;
    }
    LARGE_INTEGER T1 = KeQueryPerformanceCounter(&Freq);
    t->ElapsedUs = (ULONG)(((T1.QuadPart - T0.QuadPart) * 1000000) / (Freq.QuadPart ? Freq.QuadPart : 1));
    HdpInvalidate();
    t->Rptr = ScrRead(BC250_SCR_MEC_WB);
    t->Wptr = m_MecWptr;
    t->SdmaStatus = Rreg(BC250_CP_CPC_STATUS);
    t->FaultStatus = Rreg(BC250_CP_MEC1_INSTR_PNTR);
    return Ok ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}
