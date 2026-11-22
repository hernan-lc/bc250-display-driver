<#
.SYNOPSIS
  Installs the BC250Display driver on the BC-250 (run as Administrator, ON the BC-250).
.DESCRIPTION
  - Verifies that PCI\VEN_1002&DEV_13FE (or 143F) is present.
  - Imports the test certificate (Root + TrustedPublisher).
  - Optional: enables TESTSIGNING (-EnableTestSigning; needs a reboot and Secure Boot off).
    BitLocker is suspended for one reboot so the BCD change does not trigger recovery.
  - Disables sleep/hibernate: the driver does not support S3/S4 yet (bugcheck by design).
  - Installs the driver with pnputil.
#>
param(
  [string]$PackageDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\Release'),
  [switch]$EnableTestSigning,
  [switch]$KeepSleep
)
$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'Run this script as Administrator.' }

$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'PCI\\VEN_1002&DEV_(13FE|143F)' }
if (-not $dev) { throw 'No BC-250 GPU found (PCI\VEN_1002&DEV_13FE / 143F). Check Device Manager.' }
$dev | Format-Table Status, FriendlyName, InstanceId -AutoSize

$inf = Join-Path $PackageDir 'BC250Display.inf'
if (-not (Test-Path $inf)) { throw "$inf not found. Build first with scripts\build.ps1" }

# Test certificate
$cer = Get-ChildItem $PackageDir -Filter *.cer | Select-Object -First 1
if ($cer) {
  certutil.exe -addstore -f Root $cer.FullName | Out-Null
  certutil.exe -addstore -f TrustedPublisher $cer.FullName | Out-Null
  Write-Host "Certificate $($cer.Name) imported."
} else {
  Write-Warning 'No .cer in the package; if the driver is not signed, installation will fail.'
}

# Test signing (SystemStartOptions is language independent)
$opts = (Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control).SystemStartOptions
if ($opts -notmatch 'TESTSIGNING') {
  if ($EnableTestSigning) {
    $bl = Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction SilentlyContinue
    if ($bl -and $bl.ProtectionStatus -eq 'On') {
      Suspend-BitLocker -MountPoint $env:SystemDrive -RebootCount 1 | Out-Null
      Write-Host 'BitLocker suspended for 1 reboot (it resumes automatically).'
    }
    bcdedit.exe /set testsigning on | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'bcdedit failed' }
    Write-Warning 'TESTSIGNING enabled. Reboot and run this script again.'
    return
  }
  Write-Warning 'TESTSIGNING is off. Run with -EnableTestSigning (Secure Boot must be disabled). If you already did, reboot first.'
  return
}

# Stability: no sleep
if (-not $KeepSleep) {
  powercfg.exe /change standby-timeout-ac 0
  powercfg.exe /change standby-timeout-dc 0
  powercfg.exe /hibernate off
  Write-Host 'Sleep and hibernate disabled (the driver does not support S3/S4).'
}

pnputil.exe /add-driver $inf /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil failed ($LASTEXITCODE)" }
Write-Host 'Driver installed. Reboot if Device Manager asks for it.' -ForegroundColor Green
