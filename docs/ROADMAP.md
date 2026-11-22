# Roadmap and status

Legend: **code** = written and builds (locally and in CI); **HW-tested** = verified on a real BC-250;
⚠️ = partially working (the kernel side runs but the Windows/display integration fails).
Nothing past M1 runs unless you ask for it (`bc250ctl` with `DeveloperMode`, or by installing the
experimental `BC250Kmd` driver), so a fresh install of `BC250Display` behaves like a display-only driver.

| # | Milestone | Code | HW-tested on a BC-250 | How to test |
|---|---|---|---|---|
| 0 | Display-only on the GOP framebuffer (`BC250Display.sys`) | ✅ | ✅ 1920×1080 | `install.ps1` |
| M1 | BAR5 registers, memory layout, SMU (fw 88.6.0), GFX clock (1500 MHz) | ✅ | ✅ | `bc250ctl info` (see `docs/hw-info-m1.txt`) |
| M2 | IH ring 0 + ISR, **MSI** interrupts | ✅ | ✅ SDMA trap → IH → ISR | `bc250ctl fulltest` |
| M3 | GCVM VMID0: system aperture + flat 256 MiB GART | ✅ | ✅ SDMA writes a system page through the GART | `bc250ctl fulltest` |
| M4c | Firmware loading through the **PSP** (KM ring, TMR, LOAD_IP_FW) | ✅ | ✅ SDMA0, CE, PFP, ME, RLC loaded | `bc250ctl fulltest` |
| M4 | SDMA0 ring: WRITE_LINEAR / FENCE / TRAP | ✅ | ✅ 79 µs | `bc250ctl fulltest` |
| M4b | RLC + CP graphics ring, PM4 | ✅ | ✅ SET_UCONFIG_REG + WRITE_DATA (11 µs) | `bc250ctl fulltest` |
| M4d | MEC compute queue (HQD), MEC firmware via PSP | ✅ | ✅ WRITE_DATA + RELEASE_MEM (26 µs) | `bc250ctl fulltest` |
| M5a | Indirect buffers + fences; GPU fill into the scanout | ✅ | ✅ rectangle drawn on screen | `bc250ctl submit-test`, `fill-rect` |
| M5b | `BC250Kmd.sys`: full WDDM driver (WDDM 1.1 caps) | ✅ | ⚠️ installed 2026-11-22: `KmdBringUp=0` (step 8), but DxgKrnl cannot set a display mode (code 43 / `0x1A8` param `0xA`) | `install-kmd-safe.ps1` + `bc250kmdtest` |
| M6a | `libbc250` (escape API) + `bc250demo` | ✅ | ✅ fill+copy 64 KiB, color bars | `bc250demo` |
| M6b | `libbc250wddm` (D3DKMT winsys) + `bc250kmdtest` | ✅ | ❌ blocked by the M5b display failure | `bc250kmdtest` |
| M6c | Mesa RADV on Windows (Vulkan) | ❌ | ❌ | — |
| M7 | Direct3D via DXVK | ❌ | ❌ | — |

### Current state (2026-11-22)

`BC250Kmd` was installed on real hardware for the first time. The kernel side works:
`KmdBringUpStep=8` and `KmdBringUp=0` in the driver registry key. The display side does not:
DxgKrnl cannot enable a path, producing a black screen. The offline registry repair has been
applied (`BC250Kmd` disabled, `NOVGA` removed, fake UMD values cleared); the machine will come
up with the Microsoft Basic Display Adapter until `BC250Display` is reinstalled with
`install.ps1`. Recovery tooling and the full diagnosis are in
[BOOT-RECOVERY.md](BOOT-RECOVERY.md).

### Hardware findings (BC-250, cyan_skillfish2)

- With the latest DDI version and `DxgkDdiEscape`, dxgkrnl fails `AddDevice` (0xC0000182): the driver
  declares `DXGKDDI_INTERFACE_VERSION_WIN8`, and `DRIVERCAPS` must accept the WIN8-sized buffer.
- The microcode ports (`SDMA0_UCODE_*`, `CP_HYP_*`, RLC ucode) are **protected**: direct writes are
  dropped and read back as `FFFFFFFF`. Firmware must be loaded through the PSP (sOS is already running).
- SDMA `FENCE` needs `MTYPE=UC` (header bits 16–18) or the value stays in the GPU cache.
- The GPU does not raise the legacy INTx line: the INF requests **MSI**, and the IH uses `RPTR_REARM`.
- CP graphics: the earlier stall (`CP_STAT=80008000`, PFP busy, ring rptr 0) was resolved by
  halting RLC and soft-resetting CP/RLC before loading microcode, and by loading MEC firmware
  as well; M4b and M4d are now verified on hardware (see commits `953e165`, `40a2bae`, `eb6749b`).
- The GPU does not expose a usable display path from the kernel driver yet. With `BC250Kmd`
  active, DxgKrnl logs event 549 (FailureReason 3) and emits `0x193 VIDEO_DXGKRNL_LIVEDUMP`
  / `0x1A8 VIDEO_DXGKRNL_BLACK_SCREEN_LIVEDUMP` with parameter `0xA` ("monitor connected but
  no mode set could enable a path"). The `UserModeDriverName=bc250umd.dll` placeholder in the
  INF made this worse: that DLL does not exist, so dxgkrnl refuses to start the adapter.
  The placeholder has been removed from `package/bc250kmd.inf`.
- `NOVGA` in `SystemStartOptions` disables the Microsoft Basic Display Adapter fallback and
  turns any display-driver failure into a black screen. It is removed by the recovery tools.

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
.\scripts\install-kmd-safe.ps1            # as Administrator (auto-revert on failure; see BOOT-RECOVERY.md)
.\out\Release\bc250kmdtest.exe             # M6b: SDMA fill/copy (node 0), PM4 WRITE_DATA (node 1)
.\scripts\uninstall-kmd.ps1                # back to BC250Display (works from Safe Mode)
```

A wrong register sequence can hang the GPU. With `BC250Display` the display keeps working (scanout
does not depend on SDMA/CP); reboot before trying again. `BC250Kmd` replaces the display driver: if the
adapter fails to start you get the Microsoft Basic Display Adapter or a black screen. Use
`install-kmd-safe.ps1` (it keeps a fallback and arms an auto-revert guard); if the screen stays black,
run `scripts\recover-boot.ps1` from WinRE, or `.\scripts\uninstall-kmd.ps1` from Safe Mode (it removes
the KMD packages and reinstalls `BC250Display`). See [BOOT-RECOVERY.md](BOOT-RECOVERY.md).

## Known gaps and risks

- **No user-mode D3D driver**: `BC250Kmd` registers no `UserModeDriverName`, so the Desktop Window
  Manager cannot use the GPU; the desktop is expected to fall back to software composition.
  Presents to the primary are copied by SDMA into the GOP scanout surface (no DCN flips, no vsync).
  A placeholder `bc250umd.dll` was tried and removed: the DLL does not exist and dxgkrnl refuses
  to start an adapter that names a missing UMD.
- **GFXOFF**: M4b disables the RLC↔SMU handshake (`RLC_PG_CNTL[23]`) so GFX stays powered.
  Before M4b, GC registers may read as `0`/`FFFFFFFF` if the SMU power-gated GFX.
- **VRAM layout**: driver scratch at 64–72 MiB, `BC250Kmd` VRAM segment from 128 MiB to the end of
  BAR0/carve-out; BAR0 and the carve-out must be large enough (`bc250ctl info` shows both).
- **VMID0 only**: every context shares one GPU address space (no per-process page tables yet), so
  any submitted command buffer can reach all VRAM and GART memory. Fine for bring-up, not for
  untrusted code.
- **CP**: the MEC compute queue (HQD) now runs, but there is still no SH_MEM/VMID setup and no
  shader dispatch.
- **Paging**: VRAM↔system transfers are done with the CPU after waiting for SDMA idle.
- **Watchdog risk**: bring-up uses long busy-waits (`KmdWaitIdle(2000000)` = up to 2 s of
  `KeStallExecutionProcessor`, plus 100000-iteration MMIO polls). Watchdog live dumps were
  observed; shorten/bound these before relying on the driver interactively.

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
