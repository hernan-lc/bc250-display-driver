# Roadmap and status

Legend: **implemented** = code written and builds; **HW-tested** = verified on a real BC-250.
Everything past M1 is opt-in at runtime (`bc250ctl`) and requires the `DeveloperMode` registry
value, so a fresh install behaves exactly like the display-only driver.

| # | Milestone | Code | HW-tested | How to test |
|---|---|---|---|---|
| 0 | Display-only on the GOP framebuffer | ✅ | ✅ 1920×1080 | install the driver |
| M1 | Map BAR5 registers; read GRBM/CP/RLC/SDMA status, memory layout, SMU version, GFX clock | ✅ | ❌ | `bc250ctl info` |
| M2 | IH ring 0 + ISR (line interrupts), counters | ✅ | ❌ | `bc250ctl selftest` (TRAP step) |
| M3 | GCVM VMID0: system aperture + flat 256 MiB GART | ✅ | ❌ | `bc250ctl selftest` (GART step) |
| M4 | SDMA0: direct microcode load, GFX ring, WRITE_LINEAR / FENCE / TRAP | ✅ | ❌ | `bc250ctl selftest firmware\cyan_skillfish2_sdma.bin` |
| M5a | Command submission: indirect buffers + fences + interrupts; GPU fill into scanout | ✅ | ❌ | `bc250ctl submit-test`, `bc250ctl fill-rect 100 100 400 300 ff0000` |
| M5b | Full WDDM render KMD (memory segments, allocations, paging via SDMA, scheduling, TDR) | ❌ | ❌ | — |
| M6a | `libbc250` user-mode winsys API + `bc250demo` (UMD-style client) | ✅ | ❌ | `bc250demo` |
| M6b | Mesa RADV on Windows (Vulkan) | ❌ | ❌ | — |
| M7 | Direct3D via DXVK on top of M6b | ❌ | ❌ | — |

## Test sequence on the BC-250

```powershell
.\scripts\build.ps1
.\scripts\install.ps1                      # as Administrator; reboot if asked
.\out\Release\bc250ctl.exe info            # M1, read-only, safe
.\out\Release\bc250ctl.exe devmode on      # as Administrator
.\scripts\get-firmware.ps1
.\out\Release\bc250ctl.exe selftest firmware\cyan_skillfish2_sdma.bin   # M3 + M2 + M4
.\out\Release\bc250ctl.exe submit-test     # M5a
.\out\Release\bc250demo.exe                # M6a, draws color bars with the GPU
.\out\Release\bc250ctl.exe stop
```

`selftest` can hang the GPU if a register sequence is wrong. The display keeps working in that case
(scanout does not depend on SDMA), but reboot before trying again. Send the full output of
`bc250ctl info` and `selftest` when reporting results.

## Known risks and open questions

- **GFXOFF**: if the SMU power-gates GFX while idle, GC registers read as `0` or `FFFFFFFF`.
  `bc250ctl info` shows it; the fix is the SMU `DisallowGfxOff`-style message for this PMFW.
- **Scratch window**: the driver uses VRAM offset 64–68 MiB through BAR0. If BAR0 is smaller
  than 68 MiB or the carve-out is smaller, M2–M5 report `BC250_E_NOMEM`.
- **Firmware load type**: Linux uses direct (non-PSP) loading for `cyan_skillfish2` by default,
  which is what M4 does. Newer kernels can load through the PSP instead.
- **IH and MMHUB**: the IH ring lives in VRAM and is written through MMHUB; the driver relies on the
  MMHUB system aperture that the VBIOS programmed (shown by `bc250ctl info`).

## What M5b (full WDDM render driver) needs

Switching from `DxgkInitializeDisplayOnlyDriver` to `DxgkInitialize` with a full
`DRIVER_INITIALIZATION_DATA`:

1. **Memory model**: report segments in `DxgkDdiQueryAdapterInfo(DXGKQAITYPE_QUERYSEGMENT4)`:
   a CPU-visible VRAM segment (the carve-out through BAR0) and an aperture segment backed by the
   M3 GART. `DxgkDdiBuildPagingBuffer` emits SDMA COPY/FILL packets (already used in M5a).
2. **Submission**: `DxgkDdiSubmitCommand` = M5a `SubmitIb` without the CPU wait; the trap
   interrupt calls `DxgkCbNotifyInterrupt(DXGK_INTERRUPT_DMA_COMPLETED)` from the ISR and
   `DxgkCbNotifyDpc` from the DPC. `DxgkDdiPatch`, `DxgkDdiPreemptCommand`,
   `DxgkDdiResetFromTimeout` / `RestartFromTimeout` (halt + reload SDMA).
3. **Contexts/devices/allocations**: `CreateDevice`, `CreateContext`, `CreateAllocation`,
   `OpenAllocation`, `DescribeAllocation`, `GetStandardAllocationDriverData`.
4. **Display**: keep scanout on the GOP surface first (copy present), then program DCN 2.0.1
   (`drivers/gpu/drm/amd/display` in Linux) for real mode setting and flips.
5. **Graphics/compute engine**: RLC + CP (PFP/ME/CE/MEC) bring-up from `gfx_v10_0.c`
   (`cyan_skillfish2_{rlc,pfp,me,ce,mec}.bin`, downloaded by `get-firmware.ps1`), golden
   registers, clear state, then a compute queue (MQD/HQD). Shaders need this; SDMA only moves memory.
6. A user-mode driver must be registered in the INF (`UserModeDriverName`), otherwise the
   Desktop Window Manager cannot use the adapter.

## What M6b (Mesa RADV) needs

RADV already supports gfx1013. A Windows port needs a new winsys
(`src/amd/vulkan/winsys/`) implementing `radeon_winsys` on top of D3DKMT:
`buffer_create/map` → `D3DKMTCreateAllocation`/`D3DKMTLock2`, `cs_submit` → `D3DKMTSubmitCommand`,
fences → WDDM monitored fences, plus the PM4 path on the compute/graphics queue from M5b step 5.
`libbc250` is the prototype of that layer: it already builds and submits command buffers.
