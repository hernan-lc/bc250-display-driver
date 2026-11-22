<#
.SYNOPSIS
  Removes BC250Display and returns to the Microsoft Basic Display Adapter.
#>
$ErrorActionPreference = 'Stop'
$drivers = pnputil.exe /enum-drivers | Out-String
$blocks = $drivers -split "(\r?\n){2,}"
foreach ($b in $blocks) {
  if ($b -match 'bc250display\.inf' -and $b -match '(oem\d+\.inf)') {
    $oem = $Matches[1]
    Write-Host "Removing $oem"
    pnputil.exe /delete-driver $oem /uninstall /force
  }
}
Write-Host 'Done. Reboot to return to the Microsoft basic display driver.'
