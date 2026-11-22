// SPDX-License-Identifier: MIT
// M4c: firmware loading through the PSP (MP0, PSP v11.0.8) for the BC-250 (cyan_skillfish2).
//
// On this chip the microcode RAM ports (SDMA0_UCODE_*, CP_HYP_*, RLC_GPM_UCODE_*) are protected:
// direct writes are dropped (they read back as FFFFFFFF), which is why Linux loads cyan_skillfish2
// firmware through the PSP by default. The sOS is already running (started by the platform firmware),
// so the driver only has to: create the KM (GPCOM) ring, set up the TMR, and send LOAD_IP_FW commands.
// Sequences from amdgpu_psp.c / psp_v11_0_8.c, structures from psp_gfx_if.h (MIT).

#include "..\bdd.hxx"

#pragma code_seg("PAGE")

#define GFX_FLAG_RESPONSE               0x80000000u
#define GFX_CMD_STATUS_MASK             0x0000FFFFu
#define GFX_CMD_ID_SETUP_TMR            0x00000005u
#define GFX_CMD_ID_LOAD_IP_FW           0x00000006u
#define GFX_CMD_ID_DESTROY_TMR          0x00000007u
#define GFX_CTRL_CMD_ID_DESTROY_RINGS   0x00030000u
#define PSP_RING_TYPE_KM                2u
#define PSP_RING_SIZE                   0x1000u
#define PSP_FRAME_DW                    16u        // struct psp_gfx_rb_frame = 64 bytes

// psp_gfx_fw_type
#define GFX_FW_TYPE_CP_ME               1u
#define GFX_FW_TYPE_CP_PFP              2u
#define GFX_FW_TYPE_CP_CE               3u
#define GFX_FW_TYPE_CP_MEC              4u
#define GFX_FW_TYPE_CP_MEC_ME1          5u
#define GFX_FW_TYPE_RLC_G               8u
#define GFX_FW_TYPE_SDMA0               9u

// psp_gfx_cmd_resp layout (1024 bytes): cmd_id at +8, command union at +28, resp at +864
#define CMD_OFF_ID                      8u
#define CMD_OFF_U                       28u
#define CMD_OFF_RESP_STATUS             864u
#define CMD_OFF_RESP_FW_ADDR_LO         872u

BOOLEAN BC250_GPU::PspWait(ULONG Reg, ULONG Value, ULONG Mask, ULONG TimeoutUs)
{
    PAGED_CODE();
    for (ULONG i = 0; i < TimeoutUs; i += 10)
    {
        if ((Rreg(Reg) & Mask) == Value) return TRUE;
        KeStallExecutionProcessor(10);
    }
    return FALSE;
}

NTSTATUS BC250_GPU::PspInit()
{
    PAGED_CODE();
    if (m_PspReady) return STATUS_SUCCESS;
    NTSTATUS St = MapScratch();
    if (!NT_SUCCESS(St)) return St;

    // VRAM layout: ring/cmd/fence/fw staging inside the scratch window, TMR right after it
    // (4 MiB, naturally aligned: VRAM offset 72 MiB).
    if (BC250_PSP_TMR_OFFSET + BC250_PSP_TMR_SIZE > ((ULONGLONG)m_VramSizeMb << 20)) return STATUS_INSUFFICIENT_RESOURCES;
    for (ULONG o = 0; o < 0x3000; o += 4) ScrWrite(BC250_SCR_PSP_RING + o, 0);   // ring, cmd, fence
    HdpFlush();

    // A previous driver instance may have left its KM ring alive: destroy rings first
    // (psp_v11_0_8_ring_stop). Any response is fine; the ring may not exist.
    Wreg(BC250_MP0_SMN_C2PMSG_64, GFX_CTRL_CMD_ID_DESTROY_RINGS);
    KeStallExecutionProcessor(20000);
    PspWait(BC250_MP0_SMN_C2PMSG_64, GFX_FLAG_RESPONSE, GFX_FLAG_RESPONSE, 2000000);

    // psp_v11_0_8_ring_create (bare metal): wait for sOS ready, program the KM ring, wait for response
    if (!PspWait(BC250_MP0_SMN_C2PMSG_64, GFX_FLAG_RESPONSE, GFX_FLAG_RESPONSE | GFX_CMD_STATUS_MASK, 2000000))
        return STATUS_DEVICE_NOT_READY;
    ULONGLONG Ring = ScrMc(BC250_SCR_PSP_RING);
    Wreg(BC250_MP0_SMN_C2PMSG_69, (ULONG)Ring);
    Wreg(BC250_MP0_SMN_C2PMSG_70, (ULONG)(Ring >> 32));
    Wreg(BC250_MP0_SMN_C2PMSG_71, PSP_RING_SIZE);
    Wreg(BC250_MP0_SMN_C2PMSG_64, PSP_RING_TYPE_KM << 16);
    KeStallExecutionProcessor(20000);
    if (!PspWait(BC250_MP0_SMN_C2PMSG_64, GFX_FLAG_RESPONSE, GFX_FLAG_RESPONSE | GFX_CMD_STATUS_MASK, 2000000))
        return STATUS_IO_TIMEOUT;
    m_PspFence = 0;
    m_PspReady = TRUE;

    // psp_tmr_load: SETUP_TMR with both the GPU (MC) and the system physical address
    ULONGLONG TmrMc = m_FbStart + BC250_PSP_TMR_OFFSET;
    ULONGLONG TmrPa = McToPhys(TmrMc);
    ULONG Cmd[7] = { (ULONG)TmrMc, (ULONG)(TmrMc >> 32), (ULONG)BC250_PSP_TMR_SIZE, 1u << 1 /* virt_phy_addr */,
                     (ULONG)TmrPa, (ULONG)(TmrPa >> 32), 0 };
    St = PspCommand(GFX_CMD_ID_SETUP_TMR, Cmd, 6);
    if (St == STATUS_UNSUCCESSFUL)
    {
        // TMR still registered by a previous instance: release it and set it up again
        PspCommand(GFX_CMD_ID_DESTROY_TMR, NULL, 0);
        St = PspCommand(GFX_CMD_ID_SETUP_TMR, Cmd, 6);
    }
    if (!NT_SUCCESS(St)) { m_PspReady = FALSE; return St; }
    return STATUS_SUCCESS;
}

// Engines must already be halted: their microcode lives in the TMR.
VOID BC250_GPU::PspShutdown()
{
    PAGED_CODE();
    if (!m_PspReady) return;
    PspCommand(GFX_CMD_ID_DESTROY_TMR, NULL, 0);
    Wreg(BC250_MP0_SMN_C2PMSG_64, GFX_CTRL_CMD_ID_DESTROY_RINGS);
    KeStallExecutionProcessor(20000);
    PspWait(BC250_MP0_SMN_C2PMSG_64, GFX_FLAG_RESPONSE, GFX_FLAG_RESPONSE, 2000000);
    m_PspReady = FALSE;
    m_PspLoaded = 0;
}

// psp_cmd_submit_buf + psp_ring_cmd_submit
NTSTATUS BC250_GPU::PspCommand(ULONG CmdId, const ULONG* Args, ULONG ArgCount)
{
    PAGED_CODE();
    if (!m_PspReady) return STATUS_INVALID_DEVICE_STATE;

    for (ULONG o = 0; o < 1024; o += 4) ScrWrite(BC250_SCR_PSP_CMD + o, 0);
    ScrWrite(BC250_SCR_PSP_CMD + CMD_OFF_ID, CmdId);
    for (ULONG i = 0; i < ArgCount; ++i) ScrWrite(BC250_SCR_PSP_CMD + CMD_OFF_U + i * 4, Args[i]);

    ULONG Index = ++m_PspFence;
    ULONG Wptr = Rreg(BC250_MP0_SMN_C2PMSG_67);                       // in dwords
    ULONG FrameOff = BC250_SCR_PSP_RING + ((Wptr % (PSP_RING_SIZE / 4)) * 4);
    ULONGLONG CmdMc = ScrMc(BC250_SCR_PSP_CMD), FenceMc = ScrMc(BC250_SCR_PSP_FENCE);
    for (ULONG i = 0; i < PSP_FRAME_DW; ++i) ScrWrite(FrameOff + i * 4, 0);
    ScrWrite(FrameOff + 0,  (ULONG)CmdMc);
    ScrWrite(FrameOff + 4,  (ULONG)(CmdMc >> 32));
    ScrWrite(FrameOff + 12, (ULONG)FenceMc);
    ScrWrite(FrameOff + 16, (ULONG)(FenceMc >> 32));
    ScrWrite(FrameOff + 20, Index);
    HdpFlush();
    Wreg(BC250_MP0_SMN_C2PMSG_67, (Wptr + PSP_FRAME_DW) % (PSP_RING_SIZE / 4));

    bc_u32 Seen = 0;
    BOOLEAN Done = WaitScratch(BC250_SCR_PSP_FENCE, Index, 2000000, &Seen);
    HdpInvalidate();
    m_PspLastStatus = ScrRead(BC250_SCR_PSP_CMD + CMD_OFF_RESP_STATUS);
    m_PspLastCmd = CmdId;
    if (!Done) return STATUS_IO_TIMEOUT;
    return m_PspLastStatus == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS BC250_GPU::PspLoadFw(ULONG Engine, ULONG FwType)
{
    PAGED_CODE();
    const UCHAR* Fw = (Engine == BC250_FW_SDMA0) ? m_Fw : m_FwCp[Engine];
    ULONG FwSize = (Engine == BC250_FW_SDMA0) ? m_FwSize : m_FwCpSize[Engine];
    if (!Fw || FwSize < 32) return STATUS_INVALID_DEVICE_STATE;
    ULONG Bytes = *(const ULONG*)(Fw + 20), Off = *(const ULONG*)(Fw + 24);
    if (Off >= FwSize || Bytes > FwSize - Off || Bytes > BC250_PSP_FW_MAX || (Bytes & 3)) return STATUS_INVALID_IMAGE_FORMAT;

    return PspLoadBlob(Fw + Off, Bytes, FwType);
}

NTSTATUS BC250_GPU::PspLoadBlob(const UCHAR* Data, ULONG Bytes, ULONG FwType)
{
    PAGED_CODE();
    if (!Bytes || Bytes > BC250_PSP_FW_MAX || (Bytes & 3)) return STATUS_INVALID_IMAGE_FORMAT;
    // psp_copy_fw: stage the ucode in a 1 MiB-aligned buffer
    const ULONG* Code = (const ULONG*)Data;
    for (ULONG i = 0; i < Bytes / 4; ++i) ScrWrite(BC250_SCR_PSP_FW + i * 4, Code[i]);
    for (ULONG o = Bytes; o < ((Bytes + 0xFFF) & ~0xFFFu); o += 4) ScrWrite(BC250_SCR_PSP_FW + o, 0);
    HdpFlush();
    ULONGLONG FwMc = ScrMc(BC250_SCR_PSP_FW);
    ULONG Cmd[4] = { (ULONG)FwMc, (ULONG)(FwMc >> 32), Bytes, FwType };
    return PspCommand(GFX_CMD_ID_LOAD_IP_FW, Cmd, 4);
}

NTSTATUS BC250_GPU::PspLoadAll(BC250_TEST* t)
{
    PAGED_CODE();
    RtlZeroMemory(t, sizeof(*t));
    NTSTATUS St = PspInit();
    t->Step = 1;
    if (!NT_SUCCESS(St)) goto out;

    // engines must not run while their microcode is replaced
    if (m_SdmaReady) SdmaHalt();
    Rmw(BC250_SDMA0_F32_CNTL, 0, BC250_SDMA0_F32_CNTL__HALT_MASK);
    Rmw(BC250_CP_ME_CNTL, 0, BC250_CP_ME_CNTL__ME_HALT_MASK | BC250_CP_ME_CNTL__PFP_HALT_MASK | BC250_CP_ME_CNTL__CE_HALT_MASK);
    Rmw(BC250_RLC_CNTL, BC250_RLC_CNTL__RLC_ENABLE_F32_MASK, 0);
    Wreg(BC250_CP_MEC_CNTL, BC250_CP_MEC_CNTL__MEC_ME1_HALT_MASK | BC250_CP_MEC_CNTL__MEC_ME2_HALT_MASK);
    m_CpReady = FALSE;

    // CP/RLC may have been started earlier with unusable microcode: soft-reset them before the PSP
    // programs their instruction caches (gfx_v10_0_soft_reset: CP, CPF, CPC, CPG, GFX, RLC).
    {
        const ULONG Reset = 0x00000001u | 0x00000004u | 0x00010000u | 0x00020000u | 0x00040000u | 0x00080000u;
        Rmw(BC250_GRBM_SOFT_RESET, 0, Reset);
        (void)Rreg(BC250_GRBM_SOFT_RESET);
        KeStallExecutionProcessor(50);
        Rmw(BC250_GRBM_SOFT_RESET, Reset, 0);
        (void)Rreg(BC250_GRBM_SOFT_RESET);
        KeStallExecutionProcessor(50);
    }

    // amdgpu ucode order: SDMA0, CP_CE, CP_PFP, CP_ME, RLC_G
    t->Step = 2; if (m_Fw && !NT_SUCCESS(St = PspLoadFw(BC250_FW_SDMA0, GFX_FW_TYPE_SDMA0))) goto out;
    if (m_Fw) m_PspLoaded |= 1u << BC250_FW_SDMA0;
    t->Step = 3; if (m_FwCp[BC250_FW_CE] && !NT_SUCCESS(St = PspLoadFw(BC250_FW_CE, GFX_FW_TYPE_CP_CE))) goto out;
    t->Step = 4; if (m_FwCp[BC250_FW_PFP] && !NT_SUCCESS(St = PspLoadFw(BC250_FW_PFP, GFX_FW_TYPE_CP_PFP))) goto out;
    t->Step = 5; if (m_FwCp[BC250_FW_ME] && !NT_SUCCESS(St = PspLoadFw(BC250_FW_ME, GFX_FW_TYPE_CP_ME))) goto out;
    t->Step = 6; if (m_FwCp[BC250_FW_RLC] && !NT_SUCCESS(St = PspLoadFw(BC250_FW_RLC, GFX_FW_TYPE_RLC_G))) goto out;
    for (ULONG e = BC250_FW_RLC; e <= BC250_FW_CE; ++e) if (m_FwCp[e]) m_PspLoaded |= 1u << e;
    // MEC: main image (ucode minus jump table) + jump table (amdgpu CP_MEC1 / CP_MEC1_JT)
    if (m_FwCp[BC250_FW_MEC])
    {
        const UCHAR* Fw = m_FwCp[BC250_FW_MEC];
        ULONG FwSize = m_FwCpSize[BC250_FW_MEC];
        ULONG Bytes = *(const ULONG*)(Fw + 20), Off = *(const ULONG*)(Fw + 24);
        ULONG JtOff = *(const ULONG*)(Fw + 36), JtSize = *(const ULONG*)(Fw + 40);
        St = STATUS_INVALID_IMAGE_FORMAT;
        t->Step = 7;
        if (Off < FwSize && Bytes <= FwSize - Off && JtSize * 4 <= Bytes && (ULONGLONG)(JtOff + JtSize) * 4 <= Bytes)
        {
            St = PspLoadBlob(Fw + Off, Bytes - JtSize * 4, GFX_FW_TYPE_CP_MEC);
            t->Step = 8;
            if (NT_SUCCESS(St)) St = PspLoadBlob(Fw + Off + JtOff * 4, JtSize * 4, GFX_FW_TYPE_CP_MEC_ME1);
        }
        if (!NT_SUCCESS(St)) goto out;
        m_PspLoaded |= 1u << BC250_FW_MEC;
    }
    Wreg(BC250_CP_MEC_CNTL, BC250_CP_MEC_CNTL__MEC_ME1_HALT_MASK | BC250_CP_MEC_CNTL__MEC_ME2_HALT_MASK);  // keep halted until MecStart
    m_MecReady = FALSE;
    t->Step = 100;
out:
    t->Expected = 0;
    t->Observed = m_PspLastStatus;          // PSP response status of the last command
    t->Rptr = m_PspLastCmd;
    t->Wptr = Rreg(BC250_MP0_SMN_C2PMSG_67);
    t->SdmaStatus = Rreg(BC250_MP0_SMN_C2PMSG_64);
    t->FaultStatus = Rreg(BC250_MP0_SMN_C2PMSG_81); // sOS version
    t->TrapsBefore = m_PspLoaded;
    return St;
}
