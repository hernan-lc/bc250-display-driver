# BC250Display — Windows driver for the AMD BC-250

A community **WDDM** driver for the AMD BC-250 board
(APU *Cyan Skillfish*, gfx1013, `PCI\VEN_1002&DEV_13FE`, and the `DEV_143F` variant).

It started from Microsoft's **KMDOD** sample
([Windows-driver-samples/video/KMDOD](https://github.com/microsoft/Windows-driver-samples/tree/main/video/KMDOD), MS-PL),
a complete, proven display-only WDDM driver, and grows toward GPU access milestone by milestone.
Register offsets and init sequences are taken from the Linux `amdgpu` driver (MIT-licensed files only).

## Status

| Milestone | State | Default |
|---|---|---|
| Display-only on the UEFI GOP framebuffer | ✅ tested on hardware (1920×1080) | on |
| See [docs/ROADMAP.md](docs/ROADMAP.md) for M1–M6 | | |

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
