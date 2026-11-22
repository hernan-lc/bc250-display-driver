<#
.SYNOPSIS
  EXPERIMENTAL: replaces BC250Display (display-only) with BC250Kmd (full WDDM, SDMA render path).
.DESCRIPTION
  Run as Administrator on the BC-250, after install.ps1 has already worked (test signing on).
  - Copies cyan_skillfish2_sdma.bin to %SystemRoot%\System32\drivers\bc250\ (BC250Kmd loads it at start)
  - Removes the BC250Display package and installs BC250Kmd
  If the adapter fails to start (code 43) or the screen stays black, boot to Safe Mode and run
  scripts\uninstall-kmd.ps1 to go back to BC250Display.
#>
param(
  [string]$PackageDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\Release\kmd'),
  [string]$Firmware   = (Join-Path (Split-Path -Parent $PSScriptRoot) 'firmware\cyan_skillfish2_sdma.bin')
)
$ErrorActionPreference = 'Stop'
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'Run this script as Administrator.' }
if (-not (Test-Path $Firmware)) { throw "$Firmware not found. Run scripts\get-firmware.ps1 first." }
$inf = Join-Path $PackageDir 'BC250Kmd.inf'
if (-not (Test-Path $inf)) { throw "$inf not found. Build first with scripts\build.ps1" }
if ((Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control).SystemStartOptions -notmatch 'TESTSIGNING') {
  throw 'TESTSIGNING is off. Run scripts\install.ps1 -EnableTestSigning first.'
}

$fwDir = Join-Path $env:SystemRoot 'System32\drivers\bc250'
New-Item -ItemType Directory -Force $fwDir | Out-Null
Copy-Item $Firmware $fwDir -Force
Write-Host "Firmware copied to $fwDir"

$cer = Join-Path $PackageDir 'BC250Display.cer'
certutil.exe -addstore -f Root $cer | Out-Null
certutil.exe -addstore -f TrustedPublisher $cer | Out-Null

& (Join-Path $PSScriptRoot 'uninstall.ps1')
pnputil.exe /add-driver $inf /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil failed ($LASTEXITCODE)" }
Write-Host 'BC250Kmd installed. Reboot, then check Device Manager and "bc250ctl info".' -ForegroundColor Yellow
