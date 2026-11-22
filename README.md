# BC250Display — Windows driver for the AMD BC-250

A community **WDDM** driver for the AMD BC-250 board
(APU *Cyan Skillfish*, gfx1013, `PCI\VEN_1002&DEV_13FE`, and the `DEV_143F` variant).

It started from Microsoft's **KMDOD** sample
([Windows-driver-samples/video/KMDOD](https://github.com/microsoft/Windows-driver-samples/tree/main/video/KMDOD), MS-PL),
a complete, proven display-only WDDM driver, and grows toward GPU access milestone by milestone.
Register offsets and init sequences are taken from the Linux `amdgpu` driver (MIT-licensed files only).

## Status

| Milestone | Code | Tested on hardware |
|---|---|---|
| Display-only on the UEFI GOP framebuffer | ✅ | ✅ 1920×1080 |
| M1 registers, SMU, memory layout (`bc250ctl info`) | ✅ | not yet |
| M2 interrupts (IH ring) | ✅ | not yet |
| M3 GPU memory (GCVM + GART) | ✅ | not yet |
| M4 SDMA engine (firmware load, ring, fences) | ✅ | not yet |
| M4b RLC + CP graphics ring, PM4 | ✅ | not yet |
| M5a command submission + GPU fill into the screen | ✅ | not yet |
| M5b `BC250Kmd.sys` full WDDM driver (experimental) | ✅ | not yet |
| M6a/M6b user-mode APIs: `libbc250` (escapes), `libbc250wddm` (D3DKMT winsys) | ✅ | not yet |
| M6c Mesa RADV (Vulkan), M7 Direct3D | not started | — |

Details, test sequence and risks: [docs/ROADMAP.md](docs/ROADMAP.md).

### Layout

| Path | What |
|---|---|
| `src/` | KMDOD-based WDDM driver (`BC250Display.sys`) |
| `src/gpu/` | BC-250 hardware layer (M1–M5a, CP in `bc250_gpu_cp.cxx`) and the escape protocol |
| `src/kmd/` | `BC250Kmd.sys`: full WDDM driver (experimental, `package/bc250kmd.inf`) |
| `tools/bc250ctl/` | control and test tool |
| `umd/libbc250/` | header-only API over private escapes |
| `umd/libbc250wddm/` | header-only winsys over D3DKMT for `BC250Kmd` |
| `umd/bc250demo/`, `umd/bc250kmdtest/` | clients that build and submit command buffers |
| `tools/genregs.py` | regenerates `src/gpu/bc250_regs.h` from Linux amdgpu headers |
| `scripts/get-firmware.ps1` | downloads `cyan_skillfish2_*.bin` from linux-firmware |

Things that are **not** supported: suspend S3/S4 (the installer disables sleep), hardware mode changes,
multiple monitors.

## Build

Requirements: Visual Studio 2022 with **Desktop development with C++**,
**MSVC v143 – Spectre-mitigated libs (x64/x86)** and the **Windows Driver Kit** component.
WDK/SDK headers and tools are restored from NuGet.

```powershell
.\scripts\build.ps1 -Configuration Release
```

Output in `out\Release\`: `BC250Display.sys`, `.inf`, signed `.cat`, `.cer` and the user-mode tools.
The script creates a temporary test certificate, signs the `.sys` and the `.cat`, exports the `.cer`
and deletes the private key.

> NuGet fails with "path too long" in deep folders: clone into a short path such as `C:\src\bc250`.

### CI (GitHub Actions)

Runs only when driver files change (not for docs). Builds Release only, caches the WDK and cancels
superseded runs. Debug builds: **Actions** → *Run workflow*. Artifact `BC250Display-Release` (kept 14 days).

## Install on the BC-250

1. Disable **Secure Boot** in the BC-250 firmware (required for test-signed drivers).
2. Keep your **BitLocker recovery key** at hand; the script suspends BitLocker for one reboot.
3. PowerShell as Administrator:

```powershell
.\scripts\install.ps1 -EnableTestSigning
```

Reboot, then:

```powershell
.\scripts\install.ps1
```

4. Device Manager should show **"AMD BC-250 Display-Only Adapter"**.

To go back to the Microsoft Basic Display Adapter:

```powershell
.\scripts\uninstall.ps1
```

## Troubleshooting

- Event Viewer → System, source `BC250Display`.
- **Code 43**: there was no POST/GOP framebuffer. Make sure the firmware boots in UEFI mode with video
  output on the BC-250.
- **Black screen**: boot into Safe Mode and run `uninstall.ps1`.

## License

Derived from Microsoft Windows-driver-samples under MS-PL (`LICENSE-MS-PL.txt`).
Code ported from Linux `amdgpu` comes from MIT-licensed files; their notices are kept in the files.
