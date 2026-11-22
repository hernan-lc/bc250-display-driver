// SPDX-License-Identifier: MIT
// bc250kmdtest - exercises BC250Kmd.sys through the regular WDDM path (libbc250wddm):
//   allocations, contexts on both nodes, D3DKMTRender with patch lists, D3DKMTLock sync.
//
//   1) SDMA node: CONSTANT_FILL a 64 KiB buffer, verify from the CPU
//   2) SDMA node: COPY_LINEAR buffer A -> buffer B, verify
//   3) CP graphics node (if present): PM4 WRITE_DATA into a buffer, verify
#define BC250WDDM_IMPLEMENTATION
#include "..\libbc250wddm\bc250wddm.h"
#include <stdio.h>

static int fail(const char* what, NTSTATUS st)
{
    printf("%-34s FAILED (0x%08lX)\n", what, (unsigned long)st);
    return 1;
}

static int verify(bc250w_device* dev, bc250w_bo* bo, bckmd_u32 count, bckmd_u32 expect, const char* what)
{
    void* p = NULL;
    NTSTATUS st = bc250w_bo_map(dev, bo, &p);
    if (st) return fail(what, st);
    const bckmd_u32* d = (const bckmd_u32*)p;
    bckmd_u32 bad = 0;
    for (bckmd_u32 i = 0; i < count; ++i) if (d[i] != expect) { ++bad; }
    printf("%-34s %s (first %08X last %08X, %u mismatches)\n", what, bad ? "FAILED" : "PASSED", d[0], d[count - 1], bad);
    bc250w_bo_unmap(dev, bo);
    return bad ? 2 : 0;
}

int main(void)
{
    bc250w_device dev;
    NTSTATUS st = bc250w_open(&dev);
    if (st) { printf("BC250Kmd adapter not found or device creation failed (0x%08lX).\n"
                     "Install it with scripts\\install-kmd.ps1.\n", (unsigned long)st); return 1; }
    printf("BC250Kmd  VRAM segment %llu MiB  aperture %llu MiB  engines %s%s\n",
           dev.info.VramSegmentSize >> 20, dev.info.ApertureSize >> 20,
           (dev.info.Engines & 1) ? "SDMA " : "", (dev.info.Engines & 2) ? "CP-graphics" : "");

    const bckmd_u32 Size = 64 * 1024;
    bc250w_bo A, B, C;
    if ((st = bc250w_bo_create(&dev, Size, 0, &A)) || (st = bc250w_bo_create(&dev, Size, 0, &B)) ||
        (st = bc250w_bo_create(&dev, 4096, 0, &C)))
        return fail("create allocations", st);

    bc250w_ctx sdma;
    if ((st = bc250w_ctx_create(&dev, BC250W_NODE_SDMA, &sdma))) return fail("create SDMA context", st);

    // 1) fill A
    bc250w_emit(&sdma, 11u | (2u << 30));             // SDMA CONST_FILL, dword
    bc250w_emit_addr(&sdma, &A, 0, TRUE);
    bc250w_emit(&sdma, 0x250250AA);
    bc250w_emit(&sdma, Size - 1);
    if ((st = bc250w_submit(&sdma))) return fail("submit fill", st);
    int r = verify(&dev, &A, Size / 4, 0x250250AA, "SDMA fill (node 0)");

    // 2) copy A -> B
    bc250w_emit(&sdma, 1u);                           // SDMA COPY_LINEAR
    bc250w_emit(&sdma, Size - 1);
    bc250w_emit(&sdma, 0);
    bc250w_emit_addr(&sdma, &A, 0, FALSE);
    bc250w_emit_addr(&sdma, &B, 0, TRUE);
    if ((st = bc250w_submit(&sdma))) return fail("submit copy", st);
    r |= verify(&dev, &B, Size / 4, 0x250250AA, "SDMA copy (node 0)");

    // 3) PM4 on the graphics ring
    if (dev.info.Engines & 2)
    {
        bc250w_ctx gfx;
        if ((st = bc250w_ctx_create(&dev, BC250W_NODE_GFX, &gfx))) return fail("create CP context", st);
        bc250w_emit(&gfx, (3u << 30) | (0x37u << 8) | (6u << 16));   // PACKET3(WRITE_DATA, 6): ctrl + addr(2) + 4 data
        bc250w_emit(&gfx, (5u << 8) | (1u << 20));                   // DST_SEL memory, WR_CONFIRM
        bc250w_emit_addr(&gfx, &C, 0, TRUE);
        bc250w_emit(&gfx, 0xC0FFEE01);
        bc250w_emit(&gfx, 0xC0FFEE01);
        bc250w_emit(&gfx, 0xC0FFEE01);
        bc250w_emit(&gfx, 0xC0FFEE01);
        if ((st = bc250w_submit(&gfx))) return fail("submit PM4", st);
        r |= verify(&dev, &C, 4, 0xC0FFEE01, "PM4 WRITE_DATA (node 1)");
        bc250w_ctx_destroy(&gfx);
    }
    else
        puts("PM4 WRITE_DATA (node 1)            SKIPPED (CP firmware not installed)");

    bc250w_ctx_destroy(&sdma);
    bc250w_bo_destroy(&dev, &A); bc250w_bo_destroy(&dev, &B); bc250w_bo_destroy(&dev, &C);
    bc250w_close(&dev);
    return r;
}
