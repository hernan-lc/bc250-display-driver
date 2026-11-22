<#
.SYNOPSIS
  EXPERIMENTAL BC250Kmd install. Kept for compatibility: it now delegates to install-kmd-safe.ps1.
.DESCRIPTION
  The original version of this script deleted the BC250Display package before installing BC250Kmd.
  On hardware that left the machine with a black screen (BC250Kmd could not start, NOVGA blocked the
  Basic Display Adapter, and there was no fallback). See docs/BOOT-RECOVERY.md.
  install-kmd-safe.ps1 keeps BC250Display, removes NOVGA, backs up the SYSTEM hive and arms a
  one-shot boot guard that reverts automatically.
#>
param(
  [string]$PackageDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\Release\kmd'),
  [string]$Firmware   = (Join-Path (Split-Path -Parent $PSScriptRoot) 'firmware\cyan_skillfish2_sdma.bin')
)
& (Join-Path $PSScriptRoot 'install-kmd-safe.ps1') -PackageDir $PackageDir -Firmware $Firmware
