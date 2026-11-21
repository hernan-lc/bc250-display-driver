<#
.SYNOPSIS
  Desinstala BC250Display y vuelve al "Adaptador de pantalla basico de Microsoft".
#>
$ErrorActionPreference = 'Stop'
$drivers = pnputil.exe /enum-drivers | Out-String
$blocks = $drivers -split "(\r?\n){2,}"
foreach ($b in $blocks) {
  if ($b -match 'bc250display\.inf' -and $b -match '(oem\d+\.inf)') {
    $oem = $Matches[1]
    Write-Host "Eliminando $oem"
    pnputil.exe /delete-driver $oem /uninstall /force
  }
}
Write-Host 'Listo. Reinicia para volver al driver basico de Microsoft.'
