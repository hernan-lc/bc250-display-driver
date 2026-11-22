// SPDX-License-Identifier: MIT
// bc250demo - user-mode client of libbc250, the same way a UMD would use the winsys:
// builds SDMA command buffers on the CPU, submits them, waits for fences, checks results.
//
//   bc250demo            run the memory tests, then draw a test pattern on the screen
//   bc250demo nodraw     memory tests only
#define BC250_IMPLEMENTATION
#include "..\libbc250\bc250.h"
#include <stdio.h>
#include <string.h>

#define SCR_TEST 0x100000u   // BC250_SCR_TEST inside the driver's VRAM scratch window

static int check(const char* what, int r)
{
    printf("%-28s %s\n", what, bc250_strerror(r));
    return r;
}

int main(int argc, char** argv)
{
    bc250_device dev;
    if (check("open", bc250_open(&dev))) return 1;

    BC250_INFO info;
    if (check("get-info", bc250_get_info(&dev, &info))) return 1;
    printf("  %04X:%04X  SDMA %s  IH %s  GART %s\n", info.VendorId, info.DeviceId,
           info.SdmaReady ? "running" : "off", info.IhReady ? "on" : "off", info.GartReady ? "on" : "off");
    if (!info.SdmaReady)
    {
        puts("SDMA is not running: run 'bc250ctl selftest cyan_skillfish2_sdma.bin' first.");
        return 1;
    }

    // learn the scratch window GPU address with a NOP submission
    bc_u32 nop = 0, fence = 0, us = 0;
    bc_u64 scratch = 0;
    if (check("submit NOP", bc250_submit_ib(&dev, &nop, 1, &fence, &us, &scratch))) return 1;
    const bc_u64 A = scratch + SCR_TEST;          // 64 KiB source
    const bc_u64 B = scratch + SCR_TEST + 0x10000; // 64 KiB destination

    // 1) fill A with a pattern, 2) copy A -> B, 3) read back B
    bc_u32 ib[64];
    bc_u32 n = 0;
    n += bc250_emit_fill(ib + n, A, 0xA5A5F00D, 0x10000);
    n += bc250_emit_fill(ib + n, B, 0, 0x10000);
    n += bc250_emit_copy(ib + n, A, B, 0x10000);
    if (check("fill+copy 64 KiB", bc250_submit_ib(&dev, ib, n, &fence, &us, NULL))) return 1;
    printf("  fence %u in %u us\n", fence, us);

    bc_u32 back[4];
    if (check("read back", bc250_vram_read(&dev, SCR_TEST + 0x10000 + 0xFFF0, 4, back))) return 1;
    int ok = back[0] == 0xA5A5F00D && back[3] == 0xA5A5F00D;
    printf("  last dwords of B: %08X %08X %08X %08X -> %s\n", back[0], back[1], back[2], back[3], ok ? "PASSED" : "FAILED");
    if (!ok) return 2;

    if (argc > 1 && !_stricmp(argv[1], "nodraw")) return 0;

    // 4) test pattern on the scanout buffer, drawn by the GPU (8 vertical color bars)
    static const bc_u32 bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    const bc_u32 W = 800, H = 200, X0 = 40, Y0 = 40;
    for (bc_u32 i = 0; i < 8; ++i)
    {
        int r = bc250_fill_rect(&dev, X0 + i * (W / 8), Y0, W / 8, H, bars[i]);
        if (r) return check("fill-rect", r);
    }
    puts("draw                         OK (color bars at 40,40; the desktop repaints over them)");
    bc250_close(&dev);
    return 0;
}
