# Boot / black-screen recovery

If the machine appears not to turn on after installing `BC250Kmd`, it is almost
always a **black screen**, not a power failure: Windows boots but dxgkrnl cannot
perform a mode set (`0x1A8 VIDEO_DXGKRNL_BLACK_SCREEN_LIVEDUMP`, parameter `0xA`).

Typical cause: the experimental KMD becomes the primary display adapter
(`FeatureScore 0xFA`), `BC250Display` is disabled or removed, and `NOVGA` in the
boot options prevents the Microsoft Basic Display Adapter from taking over.

**State of this machine (2026-11-22):** the offline repair from section 1 has already
been applied. `BC250Kmd\Start=4`, `BC250Display\Start=3`, `NOVGA` removed from
`SystemStartOptions`, and the fake `bc250umd.dll` values cleared. Backup:
`C:\Windows\System32\config\SYSTEM.bc250-backup-20261122-090303`.
The BC250Display driver package itself was deleted from the DriverStore by the old
`install-kmd.ps1`, so reinstall it with `.\scripts\install.ps1` once you have a desktop.

## Three ways out, fastest first

### 1. From Linux, with the Windows partition mounted (this session)

```bash
pip install python-registry
python3 tools/repair-windows-boot.py            # dry run
python3 tools/repair-windows-boot.py --apply    # writes + timestamped backups
```

It disables `BC250Kmd`, re-enables `BC250Display`, empties the fake
`bc250umd.dll` UMD registration, removes `NOVGA` from `SystemStartOptions` and
from the BCD if present. Requires `python-registry` and read-write mounts of the
Windows partition (`/run/media/meme/WINDOWS`) and the ESP (`/run/media/meme/SYSTEM`).

### 2. From WinRE (works even with no display at all)

Boot to WinRE (Shift+Restart, recovery USB, or three forced shutdowns), open a
Command Prompt / PowerShell and run:

```cmd
powershell -ExecutionPolicy Bypass -File C:\src\bc250\scripts\recover-boot.ps1
```

Then reboot. Once you have a desktop, restore the display-only driver:

```powershell
cd C:\src\bc250
.\scripts\install.ps1
```

### 3. Prevent it next time

Use `scripts/install-kmd-safe.ps1` instead of `install-kmd.ps1`. It:

- backs up the SYSTEM hive and creates a restore point,
- keeps `BC250Display` installed as fallback,
- removes `NOVGA` so the Basic Display Adapter can be used,
- registers a one-shot `BC250-DisplayGuard` task that detects a failed adapter
  (DxgKrnl event 549 / device problem / fresh live dump) within 90 s of boot,
  reverts the driver and reboots by itself.

## Diagnosis cheat sheet

| Evidence | Where | Meaning |
|---|---|---|
| `0x193` live dump, param 1 `0x801` | `C:\Windows\LiveKernelReports\WATCHDOG\` | dxgkrnl live dump, device not ready |
| `0x1A8` live dump, param 1 `0xA` | same | monitor connected, no mode set succeeded |
| Event 549, `FailureReason=3` | `Microsoft-Windows-DxgKrnl-Admin.evtx` | adapter/DDI start failure |
| `Start=4` on `BC250Display` | SYSTEM hive | fallback driver disabled |
| `NOVGA` in `SystemStartOptions` | SYSTEM hive | Basic Display Adapter disabled |
| `UserModeDriverName=bc250umd.dll` | display class key | UMD does not exist; dxgkrnl cannot start adapter |

## Caveat

If there is truly no fan spin / no LEDs / no POST, this is **not** a driver
problem — none of the above applies. The logs in this case showed Windows
booting normally until the black screen, so the software recovery is the right
first step.
