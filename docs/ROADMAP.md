# Roadmap and status

Legend: **code** = written and builds (locally and in CI); **HW-tested** = verified on a real BC-250.
Nothing past M1 runs unless you ask for it (`bc250ctl` with `DeveloperMode`, or by installing the
experimental `BC250Kmd` driver), so a fresh install of `BC250Display` behaves like a display-only driver.

| # | Milestone | Code | HW-tested | How to test |
|---|---|---|---|---|
| 0 | Display-only on the GOP framebuffer (`BC250Display.sys`) | ✅ | ✅ 1920×1080 | `install.ps1` |
| M1 | BAR5 registers, memory layout, SMU version, GFX clock | ✅ | ❌ | `bc250ctl info` |
| M2 | IH ring 0 + ISR (line interrupts) | ✅ | ❌ | `bc250ctl fulltest` |
| M3 | GCVM VMID0: system aperture + flat 256 MiB GART | ✅ | ❌ | `bc250ctl fulltest` |
| M4 | SDMA0: direct microcode load, ring, WRITE_LINEAR / FENCE / TRAP | ✅ | ❌ | `bc250ctl fulltest` |
| M4b | RLC + CP graphics ring (PFP/ME/CE via instruction cache), clear state, PM4 tests | ✅ | ❌ | `bc250ctl fulltest` |
| M5a | Indirect buffers + fences; GPU fill into the scanout | ✅ | ❌ | `bc250ctl submit-test`, `fill-rect` |
| M5b | `BC250Kmd.sys`: full WDDM 1.2 driver (segments, allocations, paging via SDMA, 2 nodes, Present, Patch, TDR) | ✅ | ❌ | `install-kmd.ps1` + `bc250kmdtest` |
| M6a | `libbc250` (escape API) + `bc250demo` | ✅ | ❌ | `bc250demo` |
| M6b | `libbc250wddm`: winsys over D3DKMT (devices, contexts, allocations, Render with patch lists, Lock) + `bc250kmdtest` | ✅ | ❌ | `bc250kmdtest` |
| M6c | Mesa RADV built for Windows on top of `libbc250wddm` (Vulkan, `vkcube`) | ❌ | ❌ | — |
| M7 | Direct3D via DXVK on top of M6c | ❌ | ❌ | — |

## Test sequence on the BC-250

```powershell
.\scripts\build.ps1
.\scripts\get-firmware.ps1                 # cyan_skillfish2_{sdma,rlc,pfp,me,ce}.bin -> firmware\

# A) display-only driver + escape tests
.\scripts\install.ps1                      # as Administrator
.\out\Release\bc250ctl.exe info            # M1, read-only
.\out\Release\bc250ctl.exe devmode on      # as Administrator
.\out\Release\bc250ctl.exe fulltest firmware   # M3 + M2 + M4 + M4b
.\out\Release\bc250ctl.exe submit-test     # M5a
.\out\Release\bc250demo.exe                # M6a, color bars drawn by the GPU

# B) experimental full WDDM driver (reboot between steps)
.\scripts\install-kmd.ps1                  # as Administrator
.\out\Release\bc250kmdtest.exe             # M6b: SDMA fill/copy (node 0), PM4 WRITE_DATA (node 1)
.\scripts\uninstall-kmd.ps1                # back to BC250Display (works from Safe Mode)
```

A wrong register sequence can hang the GPU. With `BC250Display` the display keeps working (scanout
does not depend on SDMA/CP); reboot before trying again. `BC250Kmd` replaces the display driver: if the
adapter fails to start you get the Microsoft Basic Display Adapter or a black screen, so keep
`uninstall-kmd.ps1` at hand and know how to reach Safe Mode.

## Known gaps and risks

- **No user-mode D3D driver**: `BC250Kmd` registers no `UserModeDriverName`, so the Desktop Window
  Manager cannot use the GPU; the desktop is expected to fall back to software composition.
  Presents to the primary are copied by SDMA into the GOP scanout surface (no DCN flips, no vsync).
- **GFXOFF**: M4b disables the RLC↔SMU handshake (`RLC_PG_CNTL[23]`) so GFX stays powered.
  Before M4b, GC registers may read as `0`/`FFFFFFFF` if the SMU power-gated GFX.
- **VRAM layout**: driver scratch at 64–72 MiB, `BC250Kmd` VRAM segment from 128 MiB to the end of
  BAR0/carve-out; BAR0 and the carve-out must be large enough (`bc250ctl info` shows both).
- **VMID0 only**: every context shares one GPU address space (no per-process page tables yet), so
  any submitted command buffer can reach all VRAM and GART memory. Fine for bring-up, not for
  untrusted code.
- **CP**: graphics ring only, no compute (MEC/HQD), no SH_MEM/VMID setup, no shader dispatch yet.
- **Paging**: VRAM↔system transfers are done with the CPU after waiting for SDMA idle.

## What M6c (Mesa RADV on Windows) still needs

RADV already supports gfx1013. On top of `libbc250wddm`:

| `radeon_winsys` | `libbc250wddm` |
|---|---|
| `buffer_create` / `buffer_destroy` | `bc250w_bo_create` / `bc250w_bo_destroy` |
| `buffer_map` / `buffer_unmap` | `bc250w_bo_map` / `bc250w_bo_unmap` |
| `ctx_create` | `bc250w_ctx_create(node)` |
| `cs_add_buffer`, relocations | `bc250w_use_bo` / `bc250w_emit_addr` (patch list) |
| `cs_submit` | `bc250w_submit` (`D3DKMTRender`) |
| `ctx_wait_idle`, fences | `D3DKMTLock` today; WDDM monitored fences need a WDDM 2.x KMD |

Kernel work before RADV can run shaders: compute queue (MEC firmware, HQD/MQD), SH_MEM_CONFIG and
APE bases, per-process VMIDs with multi-level page tables (WDDM 2.0 GpuMmu), and a Windows build of
Mesa with the new winsys (`src/amd/vulkan/winsys/`), the RADV ICD JSON and loader registration.
