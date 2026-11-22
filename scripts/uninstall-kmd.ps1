<#
.SYNOPSIS
  Removes BC250Kmd and reinstalls the display-only BC250Display (works from Safe Mode too).
#>
$ErrorActionPreference = 'Stop'
$blocks = (pnputil.exe /enum-drivers | Out-String) -split "(\r?\n){2,}"
foreach ($b in $blocks) {
  if ($b -match 'bc250kmd\.inf' -and $b -match '(oem\d+\.inf)') {
    Write-Host "Removing $($Matches[1])"
    pnputil.exe /delete-driver $Matches[1] /uninstall /force
  }
}
$dod = Join-Path (Split-Path -Parent $PSScriptRoot) 'out\Release\BC250Display.inf'
if (Test-Path $dod) { pnputil.exe /add-driver $dod /install }
Write-Host 'Done. Reboot.'
