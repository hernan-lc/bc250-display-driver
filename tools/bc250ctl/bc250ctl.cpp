// SPDX-License-Identifier: MIT
// bc250ctl - talks to BC250Display.sys through D3DKMTEscape.
//
//   bc250ctl info                         M1: identity, memory layout, status, SMU
//   bc250ctl read <reg>                   M1 (dev): read MMIO dword (hex dword offset)
//   bc250ctl write <reg> <value>          M1 (dev): write MMIO dword
//   bc250ctl smu <msg> [arg]              M1: SMU message (non read-only ones need dev mode)
//   bc250ctl gart-init                    M3 (dev): program GCVM VMID0 + GART
//   bc250ctl ih-init                      M2 (dev): enable IH ring
//   bc250ctl ih-status                    M2: interrupt counters
//   bc250ctl sdma-start <sdma.bin>        M4 (dev): upload SDMA0 firmware and start the ring
//   bc250ctl sdma-test                    M4 (dev): VRAM write, GART write, fence, trap
//   bc250ctl selftest <sdma.bin>          M2+M3+M4 in order
//   bc250ctl stop                         halt SDMA, disable IH
//   bc250ctl devmode on|off               set DeveloperMode for the adapter (Administrator)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <setupapi.h>
#include <devguid.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "..\..\src\gpu\bc250_escape.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "setupapi.lib")

static D3DKMT_HANDLE g_Adapter = 0;

static const char* StatusName(int s)
{
    switch (s)
    {
    case BC250_OK: return "OK";
    case BC250_E_BADCMD: return "unknown command";
    case BC250_E_DENIED: return "denied (DeveloperMode is off: run 'bc250ctl devmode on' as Administrator)";
    case BC250_E_NOHW: return "register BAR not mapped";
    case BC250_E_RANGE: return "out of range / bad parameter";
    case BC250_E_TIMEOUT: return "timeout";
    case BC250_E_STATE: return "prerequisite missing";
    case BC250_E_NOMEM: return "out of memory / VRAM window unavailable";
    case BC250_E_SMU: return "SMU rejected the message";
    case BC250_E_VERIFY: return "verification failed";
    }
    return "?";
}

static bool Escape(D3DKMT_HANDLE hAdapter, BC250_ESCAPE& e, bool quiet = false)
{
    e.Header.Magic = BC250_ESC_MAGIC;
    e.Header.Version = BC250_ESC_VERSION;
    D3DKMT_ESCAPE Esc = {};
    Esc.hAdapter = hAdapter;
    Esc.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    Esc.pPrivateDriverData = &e;
    Esc.PrivateDriverDataSize = sizeof(e);
    NTSTATUS St = D3DKMTEscape(&Esc);
    if (St != 0)
    {
        if (!quiet) fprintf(stderr, "D3DKMTEscape failed: 0x%08lX\n", (unsigned long)St);
        return false;
    }
    return true;
}

static bool FindAdapter()
{
    D3DKMT_ENUMADAPTERS2 En = {};
    if (D3DKMTEnumAdapters2(&En) != 0 || !En.NumAdapters) return false;
    std::vector<D3DKMT_ADAPTERINFO> Infos(En.NumAdapters);
    En.pAdapters = Infos.data();
    if (D3DKMTEnumAdapters2(&En) != 0) return false;
    for (ULONG i = 0; i < En.NumAdapters; ++i)
    {
        BC250_ESCAPE* e = (BC250_ESCAPE*)calloc(1, sizeof(BC250_ESCAPE));
        e->Header.Command = BC250_CMD_GET_INFO;
        bool Ok = Escape(Infos[i].hAdapter, *e, true) && e->Header.Status == BC250_OK && e->u.Info.VendorId == 0x1002;
        free(e);
        if (Ok && !g_Adapter) g_Adapter = Infos[i].hAdapter;
        else
        {
            D3DKMT_CLOSEADAPTER Close = { Infos[i].hAdapter };
            D3DKMTCloseAdapter(&Close);
        }
    }
    return g_Adapter != 0;
}

static BC250_ESCAPE* NewEsc(ULONG Cmd)
{
    BC250_ESCAPE* e = (BC250_ESCAPE*)calloc(1, sizeof(BC250_ESCAPE));
    e->Header.Command = Cmd;
    return e;
}

static int Run(BC250_ESCAPE* e, const char* What)
{
    if (!Escape(g_Adapter, *e)) return 1;
    if (e->Header.Status != BC250_OK)
    {
        printf("%-12s FAILED: %s (%d)\n", What, StatusName(e->Header.Status), e->Header.Status);
        return 2;
    }
    printf("%-12s OK\n", What);
    return 0;
}

static int CmdInfo()
{
    BC250_ESCAPE* e = NewEsc(BC250_CMD_GET_INFO);
    if (!Escape(g_Adapter, *e)) return 1;
    const BC250_INFO& i = e->u.Info;
    printf("Device          %04X:%04X subsys %08X rev %02X\n", i.VendorId, i.DeviceId, i.SubsysId, i.Revision);
    printf("DeveloperMode   %s\n", i.DeveloperMode ? "on" : "off");
    printf("BAR0 VRAM       %016llX  %llu MiB\n", i.Vram.PhysAddr, i.Vram.Length >> 20);
    printf("BAR2 doorbell   %016llX  %llu KiB\n", i.Doorbell.PhysAddr, i.Doorbell.Length >> 10);
    printf("BAR5 MMIO       %016llX  %llu KiB  %s\n", i.Mmio.PhysAddr, i.Mmio.Length >> 10, i.MmioMapped ? "mapped" : "NOT mapped");
    printf("GOP framebuffer %016llX  %u KiB\n", i.FramebufferPhys, i.FramebufferSize >> 10);
    if (!i.MmioMapped) return 0;
    printf("VRAM size       %u MiB (RCC_CONFIG_MEMSIZE)\n", i.VramSizeMb);
    printf("GC  FB          %012llX - %012llX  offset(phys) %012llX\n", i.GcFbStart, i.GcFbEnd, i.GcFbOffset);
    printf("MM  FB          %012llX - %012llX\n", i.MmFbStart, i.MmFbEnd);
    printf("GC  sys apert.  %012llX - %012llX\n", i.GcSysApertureLow, i.GcSysApertureHigh);
    printf("MM  sys apert.  %012llX - %012llX\n", i.MmSysApertureLow, i.MmSysApertureHigh);
    printf("GRBM_STATUS     %08X  (bit31 GUI_ACTIVE=%u)\n", i.GrbmStatus, (i.GrbmStatus >> 31) & 1);
    printf("GRBM_STATUS2    %08X\n", i.GrbmStatus2);
    printf("CP_STAT         %08X\n", i.CpStat);
    printf("RLC_STAT        %08X\n", i.RlcStat);
    printf("GB_ADDR_CONFIG  %08X  (golden 00100044)\n", i.GbAddrConfig);
    printf("SDMA0 STATUS    %08X  F32_CNTL %08X (HALT=%u)\n", i.SdmaStatus, i.SdmaF32Cntl, i.SdmaF32Cntl & 1);
    printf("IH_RB_CNTL      %08X\n", i.IhRbCntl);
    printf("THERMAL raw     %08X  CTF_TEMP=%u\n", i.ThermalRaw, (i.ThermalRaw >> 9) & 0x1FF);
    if (i.SmuVersion)
        printf("SMU firmware    %u.%u.%u (0x%08X)\n", (i.SmuVersion >> 16) & 0xFF, (i.SmuVersion >> 8) & 0xFF, i.SmuVersion & 0xFF, i.SmuVersion);
    else
        printf("SMU firmware    no answer\n");
    printf("GFX clock       %u (GetGfxFrequency)\n", i.GfxClkMhz);
    printf("Scratch window  %s  MC %012llX size %llu KiB\n", i.ScratchSize ? "mapped" : "not mapped", i.ScratchMc, i.ScratchSize >> 10);
    printf("GART            %s  %012llX - %012llX\n", i.GartReady ? "ready" : "off", i.GartStart, i.GartEnd);
    printf("IH              %s  interrupts %llu\n", i.IhReady ? "ready" : "off", i.InterruptCount);
    printf("SDMA0           %s\n", i.SdmaReady ? "running" : "off");
    return 0;
}

static int CmdIhStatus()
{
    BC250_ESCAPE* e = NewEsc(BC250_CMD_IH_STATUS);
    if (!Escape(g_Adapter, *e)) return 1;
    const BC250_IH_STATUS& s = e->u.Ih;
    printf("interrupts %llu  entries %llu  sdma traps %llu  overflow %u\n", s.InterruptCount, s.EntryCount, s.SdmaTrapCount, s.Overflow);
    printf("last client %u src %u data %08X  rptr %X wptr %X\n", s.LastClientId, s.LastSourceId, s.LastSrcData, s.Rptr, s.Wptr);
    return 0;
}

static int CmdSdmaStart(const char* Path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, Path, "rb") || !f) { fprintf(stderr, "cannot open %s\n", Path); return 1; }
    std::vector<unsigned char> Fw;
    unsigned char Buf[4096];
    size_t n;
    while ((n = fread(Buf, 1, sizeof(Buf), f)) > 0) Fw.insert(Fw.end(), Buf, Buf + n);
    fclose(f);
    printf("firmware        %s (%zu bytes)\n", Path, Fw.size());

    for (size_t Off = 0; Off < Fw.size(); Off += BC250_FW_CHUNK)
    {
        BC250_ESCAPE* e = NewEsc(BC250_CMD_FW_UPLOAD);
        e->u.Fw.Engine = BC250_FW_SDMA0;
        e->u.Fw.TotalSize = (bc_u32)Fw.size();
        e->u.Fw.Offset = (bc_u32)Off;
        e->u.Fw.ChunkSize = (bc_u32)min((size_t)BC250_FW_CHUNK, Fw.size() - Off);
        memcpy(e->u.Fw.Data, &Fw[Off], e->u.Fw.ChunkSize);
        if (!Escape(g_Adapter, *e) || e->Header.Status != BC250_OK)
        {
            printf("fw-upload FAILED at %zu: %s\n", Off, StatusName(e->Header.Status));
            free(e);
            return 2;
        }
        free(e);
    }
    printf("fw-upload    OK\n");
    return Run(NewEsc(BC250_CMD_SDMA_START), "sdma-start");
}

static int CmdSdmaTest()
{
    BC250_ESCAPE* e = NewEsc(BC250_CMD_SDMA_TEST);
    if (!Escape(g_Adapter, *e)) return 1;
    const BC250_TEST& t = e->u.Test;
    static const char* Steps[] = { "", "VRAM write (WRITE_LINEAR)", "GART write to system page", "FENCE", "TRAP -> IH interrupt" };
    if (e->Header.Status == BC250_OK)
        printf("sdma-test    PASSED (%u us)\n", t.ElapsedUs);
    else
        printf("sdma-test    FAILED at step %u (%s): %s\n", t.Step, t.Step < 5 ? Steps[t.Step] : "?", StatusName(e->Header.Status));
    printf("  expected %08X observed %08X  rptr %X wptr %X  SDMA status %08X  VM fault %08X\n",
           t.Expected, t.Observed, t.Rptr, t.Wptr, t.SdmaStatus, t.FaultStatus);
    if (t.TrapsAfter || t.TrapsBefore) printf("  traps %u -> %u\n", t.TrapsBefore, t.TrapsAfter);
    return e->Header.Status == BC250_OK ? 0 : 2;
}

static int CmdDevMode(bool On)
{
    // DeveloperMode lives in the adapter's driver key: HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968...}\NNNN
    HDEVINFO h = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return 1;
    SP_DEVINFO_DATA d = { sizeof(d) };
    int Found = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &d); ++i)
    {
        wchar_t Id[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(h, &d, Id, 512, nullptr)) continue;
        if (!wcsstr(Id, L"VEN_1002&DEV_13FE") && !wcsstr(Id, L"VEN_1002&DEV_143F")) continue;
        HKEY k = SetupDiOpenDevRegKey(h, &d, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_SET_VALUE);
        if (k == INVALID_HANDLE_VALUE) { fprintf(stderr, "cannot open driver key (run as Administrator)\n"); continue; }
        DWORD v = On ? 1 : 0;
        LONG r = RegSetValueExW(k, L"DeveloperMode", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
        if (r == ERROR_SUCCESS) { printf("DeveloperMode = %lu for %ls\n", v, Id); ++Found; }
        else fprintf(stderr, "RegSetValueEx failed (%ld) - run as Administrator\n", r);
    }
    SetupDiDestroyDeviceInfoList(h);
    return Found ? 0 : 1;
}

static void Usage()
{
    puts("bc250ctl info | read <reg> | write <reg> <val> | smu <msg> [arg] | gart-init | ih-init | ih-status\n"
         "         sdma-start <cyan_skillfish2_sdma.bin> | sdma-test | selftest <sdma.bin> | stop | devmode on|off");
}

int main(int argc, char** argv)
{
    if (argc < 2) { Usage(); return 1; }
    const char* c = argv[1];
    if (!_stricmp(c, "devmode") && argc >= 3) return CmdDevMode(!_stricmp(argv[2], "on"));

    if (!FindAdapter()) { fprintf(stderr, "BC250Display adapter not found (is the driver installed?)\n"); return 1; }

    if (!_stricmp(c, "info")) return CmdInfo();
    if (!_stricmp(c, "ih-status")) return CmdIhStatus();
    if (!_stricmp(c, "gart-init")) return Run(NewEsc(BC250_CMD_GART_INIT), "gart-init");
    if (!_stricmp(c, "ih-init")) return Run(NewEsc(BC250_CMD_IH_INIT), "ih-init");
    if (!_stricmp(c, "stop")) return Run(NewEsc(BC250_CMD_STOP_ALL), "stop");
    if (!_stricmp(c, "sdma-test")) return CmdSdmaTest();
    if (!_stricmp(c, "sdma-start") && argc >= 3) return CmdSdmaStart(argv[2]);
    if (!_stricmp(c, "read") && argc >= 3)
    {
        BC250_ESCAPE* e = NewEsc(BC250_CMD_READ_REG);
        e->u.Reg.Offset = strtoul(argv[2], nullptr, 16);
        if (Run(e, "read")) return 2;
        printf("[%05X] = %08X\n", e->u.Reg.Offset, e->u.Reg.Value);
        return 0;
    }
    if (!_stricmp(c, "write") && argc >= 4)
    {
        BC250_ESCAPE* e = NewEsc(BC250_CMD_WRITE_REG);
        e->u.Reg.Offset = strtoul(argv[2], nullptr, 16);
        e->u.Reg.Value = strtoul(argv[3], nullptr, 16);
        if (Run(e, "write")) return 2;
        printf("[%05X] now %08X\n", e->u.Reg.Offset, e->u.Reg.Value);
        return 0;
    }
    if (!_stricmp(c, "smu") && argc >= 3)
    {
        BC250_ESCAPE* e = NewEsc(BC250_CMD_SMU_MSG);
        e->u.Smu.Message = strtoul(argv[2], nullptr, 0);
        e->u.Smu.Argument = argc >= 4 ? strtoul(argv[3], nullptr, 0) : 0;
        int r = Run(e, "smu");
        printf("response %X output %08X (%u)\n", e->u.Smu.Response, e->u.Smu.Output, e->u.Smu.Output);
        return r;
    }
    if (!_stricmp(c, "selftest") && argc >= 3)
    {
        printf("== M3 GART ==\n");
        if (Run(NewEsc(BC250_CMD_GART_INIT), "gart-init")) return 2;
        if (Run(NewEsc(BC250_CMD_GART_TEST), "gart-map")) return 2;
        printf("== M2 IH ==\n");
        if (Run(NewEsc(BC250_CMD_IH_INIT), "ih-init")) return 2;
        printf("== M4 SDMA ==\n");
        if (CmdSdmaStart(argv[2])) return 2;
        int r = CmdSdmaTest();
        CmdIhStatus();
        return r;
    }
    Usage();
    return 1;
}
