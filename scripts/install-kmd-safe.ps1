<#
.SYNOPSIS
  Safe variant of install-kmd.ps1: installs BC250Kmd but keeps a way back.

.DESCRIPTION
  Differences vs. install-kmd.ps1:
    - Backs up the SYSTEM hive and creates a system restore point.
    - Does NOT delete the BC250Display driver package (it stays as fallback).
    - Removes NOVGA from the BCD so the Microsoft Basic Display Adapter can
      take over if the new driver cannot set a display mode.
    - Registers a ONE-SHOT boot guard (guard-display.ps1) that reverts the
      driver automatically if DxgKrnl fails again after the reboot.

  If the guard itself fails, boot to WinRE and run recover-boot.ps1.
#>
param(
    [string]$PackageDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\Release\kmd'),
    [string]$Firmware   = (Join-Path (Split-Path -Parent $PSScriptRoot) 'firmware\cyan_skillfish2_sdma.bin')
)
$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'Run this script as Administrator.' }

$inf = Join-Path $PackageDir 'BC250Kmd.inf'
if (-not (Test-Path $inf)) { throw "$inf not found. Build first with scripts\build.ps1" }
if (-not (Test-Path $Firmware)) { throw "$Firmware not found. Run scripts\get-firmware.ps1 first." }
if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions -notmatch 'TESTSIGNING') {
    throw 'TESTSIGNING is off. Run scripts\install.ps1 -EnableTestSigning first.'
}

# 1. Backup + restore point
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$hive = "$env:SystemRoot\System32\config\SYSTEM"
Copy-Item $hive "$hive.bc250-$stamp.bak" -Force
Write-Host "SYSTEM hive backed up to $hive.bc250-$stamp.bak"
try {
    Checkpoint-Computer -Description "Before BC250Kmd ($stamp)" -RestorePointType 'MODIFY_SETTINGS' -ErrorAction Stop | Out-Null
    Write-Host 'Restore point created.'
}
catch { Write-Warning "Could not create restore point: $_" }

# 2. Make the Basic Display Adapter available as the last-resort fallback
bcdedit.exe /deletevalue `{current`} novga 2>$null | Out-Null
$opts = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions
if ($opts -match 'NOVGA') {
    $new = ($opts -replace '(?i)NOVGA', '' -replace '\s+', ' ').Trim()
    Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions -Value $new -Type String
    Write-Host "SystemStartOptions: NOVGA removed"
}

# 3. Firmware + certificate (same as install-kmd.ps1)
$fwDir = Join-Path $env:SystemRoot 'System32\drivers\bc250'
New-Item -ItemType Directory -Force $fwDir | Out-Null
Copy-Item $Firmware $fwDir -Force
foreach ($n in 'rlc', 'pfp', 'me', 'ce', 'mec') {
    $cp = Join-Path (Split-Path $Firmware) "cyan_skillfish2_$n.bin"
    if (Test-Path $cp) { Copy-Item $cp $fwDir -Force }
}
$cer = Get-ChildItem $PackageDir -Filter '*.cer' | Select-Object -First 1
if ($cer) {
    certutil.exe -addstore -f Root $cer.FullName | Out-Null
    certutil.exe -addstore -f TrustedPublisher $cer.FullName | Out-Null
}

# 4. Install the driver. NOTE: we deliberately keep BC250Display installed.
pnputil.exe /add-driver $inf /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil failed ($LASTEXITCODE)" }

# 5. One-shot boot guard: revert automatically if the adapter fails again
$guard = Join-Path $PSScriptRoot 'guard-display.ps1'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$guard`""
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 20)
Unregister-ScheduledTask -TaskName 'BC250-DisplayGuard' -Confirm:$false -ErrorAction SilentlyContinue
Register-ScheduledTask -TaskName 'BC250-DisplayGuard' -Action $action -Trigger $trigger `
    -Principal $principal -Settings $settings -Description 'Auto-revert BC250Kmd if display fails after install' | Out-Null
Write-Host 'Boot guard registered (one shot).'

Write-Host @'

BC250Kmd installed safely. Reboot now.

If the screen stays black for more than ~90 s, the guard should disable the
driver and reboot automatically. If it does not, boot to WinRE and run:

  powershell -ExecutionPolicy Bypass -File C:\src\bc250\scripts\recover-boot.ps1

'@
