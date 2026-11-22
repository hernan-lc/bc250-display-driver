// SPDX-License-Identifier: MIT
// Private escape protocol between BC250Display.sys and bc250ctl.exe (D3DKMTEscape).
// Shared by kernel and user mode: plain C, fixed-size types only.
#pragma once

// MSVC built-in sized types: usable from both kernel and user mode without CRT headers.
typedef unsigned __int8  bc_u8;
typedef unsigned __int32 bc_u32;
typedef signed   __int32 bc_i32;
typedef unsigned __int64 bc_u64;

#define BC250_ESC_MAGIC    0x30353242u   // 'B250'
#define BC250_ESC_VERSION  2u

// Commands. "DEV" = requires the DeveloperMode registry value (set by an administrator).
enum BC250_CMD {
    BC250_CMD_GET_INFO       = 1,   // M1: identity, BARs, memory layout, status registers, SMU
    BC250_CMD_READ_REG       = 2,   // M1 DEV: read one MMIO dword
    BC250_CMD_WRITE_REG      = 3,   // M1 DEV: write one MMIO dword
    BC250_CMD_SMU_MSG        = 4,   // M1 DEV: send a raw SMU message (read-only messages are always allowed)
    BC250_CMD_FW_UPLOAD      = 5,   // M4 DEV: upload a firmware blob in chunks
    BC250_CMD_GART_INIT      = 6,   // M3 DEV: program GCVM (VMID0, system aperture, GART page table)
    BC250_CMD_GART_TEST      = 7,   // M3 DEV: map a system page into GART (test is completed by SDMA)
    BC250_CMD_IH_INIT        = 8,   // M2 DEV: enable the interrupt handler ring
    BC250_CMD_IH_STATUS      = 9,   // M2: interrupt counters
    BC250_CMD_SDMA_START     = 10,  // M4 DEV: load SDMA0 microcode and start the GFX ring
    BC250_CMD_SDMA_TEST      = 11,  // M4 DEV: WRITE_LINEAR to VRAM and to a GART page, FENCE, TRAP
    BC250_CMD_STOP_ALL       = 12,  // DEV: halt SDMA, disable IH (GART stays, it is harmless)
    BC250_CMD_VRAM_READ      = 13,  // DEV: read dwords from the driver's VRAM scratch window
};

// Status codes returned in BC250_ESC_HEADER::Status (0 = OK, negative = error)
#define BC250_OK               0
#define BC250_E_BADCMD        -1
#define BC250_E_DENIED        -2   // DeveloperMode is off
#define BC250_E_NOHW          -3   // MMIO BAR not mapped
#define BC250_E_RANGE         -4
#define BC250_E_TIMEOUT       -5
#define BC250_E_STATE         -6   // prerequisite step missing
#define BC250_E_NOMEM         -7
#define BC250_E_SMU           -8
#define BC250_E_VERIFY        -9   // the GPU did not produce the expected result

#define BC250_FW_SDMA0        0u
#define BC250_FW_CHUNK        4096u

#pragma pack(push, 8)

typedef struct BC250_ESC_HEADER {
    bc_u32 Magic;
    bc_u32 Version;
    bc_u32 Command;
    bc_i32  Status;
} BC250_ESC_HEADER;

typedef struct BC250_BAR {
    bc_u64 PhysAddr;
    bc_u64 Length;
} BC250_BAR;

typedef struct BC250_INFO {
    bc_u32 VendorId, DeviceId, SubsysId, Revision;
    bc_u32 DeveloperMode;
    bc_u32 MmioMapped;
    BC250_BAR Vram;        // BAR0, CPU-visible VRAM aperture
    BC250_BAR Doorbell;    // BAR2
    BC250_BAR Mmio;        // BAR5, registers
    bc_u64 GcFbStart, GcFbEnd, GcFbOffset;    // GPU MC addresses / physical base of the carve-out
    bc_u64 MmFbStart, MmFbEnd;
    bc_u64 GcSysApertureLow, GcSysApertureHigh;
    bc_u64 MmSysApertureLow, MmSysApertureHigh;
    bc_u32 VramSizeMb;
    bc_u32 GrbmStatus, GrbmStatus2, CpStat, RlcStat, GbAddrConfig;
    bc_u32 SdmaStatus, SdmaF32Cntl, IhRbCntl;
    bc_u32 SmuVersion;       // 0 if the SMU did not answer
    bc_u32 GfxClkMhz;        // PPSMC_MSG_GetGfxFrequency
    bc_u32 ThermalRaw;       // CG_MULT_THERMAL_STATUS (CTF_TEMP may be 0 on APUs)
    bc_u64 ScratchMc;        // GPU address of the driver's VRAM scratch window (0 = none)
    bc_u64 ScratchSize;
    bc_u64 GartStart, GartEnd;
    bc_u32 GartReady, IhReady, SdmaReady, Reserved0;
    bc_u64 InterruptCount;
    bc_u64 FramebufferPhys;  // POST/GOP framebuffer
    bc_u32 FramebufferSize;
    bc_u32 Reserved1;
} BC250_INFO;

typedef struct BC250_REG {
    bc_u32 Offset;   // dword offset
    bc_u32 Value;
} BC250_REG;

typedef struct BC250_SMU {
    bc_u32 Message;
    bc_u32 Argument;
    bc_u32 Response;  // PPSMC_Result_*
    bc_u32 Output;    // C2PMSG_82 after completion
} BC250_SMU;

typedef struct BC250_FW {
    bc_u32 Engine;     // BC250_FW_*
    bc_u32 TotalSize;  // bytes of the whole .bin
    bc_u32 Offset;     // offset of this chunk
    bc_u32 ChunkSize;  // <= BC250_FW_CHUNK
    bc_u8  Data[BC250_FW_CHUNK];
} BC250_FW;

typedef struct BC250_IH_STATUS {
    bc_u64 InterruptCount;   // ISR calls that were ours
    bc_u64 EntryCount;       // IH ring entries consumed
    bc_u64 SdmaTrapCount;    // client SDMA0, src TRAP
    bc_u32 LastClientId, LastSourceId, LastSrcData;
    bc_u32 Rptr, Wptr, Overflow;
} BC250_IH_STATUS;

typedef struct BC250_TEST {
    bc_u32 Step;        // last step reached (for diagnostics)
    bc_u32 Expected;
    bc_u32 Observed;
    bc_u32 Rptr, Wptr;
    bc_u32 SdmaStatus;
    bc_u32 FaultStatus; // GCVM_L2_PROTECTION_FAULT_STATUS
    bc_u32 ElapsedUs;
    bc_u32 TrapsBefore, TrapsAfter;
} BC250_TEST;

typedef struct BC250_VRAM {
    bc_u32 Offset;      // byte offset in the scratch window
    bc_u32 Count;       // dwords, <= 64
    bc_u32 Data[64];
} BC250_VRAM;

typedef struct BC250_ESCAPE {
    BC250_ESC_HEADER Header;
    union {
        BC250_INFO      Info;
        BC250_REG       Reg;
        BC250_SMU       Smu;
        BC250_FW        Fw;
        BC250_IH_STATUS Ih;
        BC250_TEST      Test;
        BC250_VRAM      Vram;
    } u;
} BC250_ESCAPE;

#pragma pack(pop)
