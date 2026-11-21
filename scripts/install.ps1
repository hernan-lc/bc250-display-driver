<#
.SYNOPSIS
  Instala el driver BC250Display en la BC-250 (ejecutar como Administrador, EN LA BC-250).
.DESCRIPTION
  - Verifica que exista el dispositivo PCI\VEN_1002&DEV_13FE (o 143F).
  - Importa el certificado de prueba (Root + TrustedPublisher).
  - Opcional: activa TESTSIGNING (-EnableTestSigning, requiere reinicio y Secure Boot desactivado).
  - Desactiva suspension/hibernacion: el driver display-only NO soporta S3 (bugcheck por diseno).
  - Instala el driver con pnputil.
#>
param(
  [string]$PackageDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\Release'),
  [switch]$EnableTestSigning,
  [switch]$KeepSleep
)
$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'Ejecuta este script como Administrador.' }

$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'PCI\\VEN_1002&DEV_(13FE|143F)' }
if (-not $dev) { throw 'No se encontro una GPU BC-250 (PCI\VEN_1002&DEV_13FE / 143F). Revisa el Administrador de dispositivos.' }
$dev | Format-Table Status, FriendlyName, InstanceId -AutoSize

$inf = Join-Path $PackageDir 'BC250Display.inf'
if (-not (Test-Path $inf)) { throw "No existe $inf. Compila primero con scripts\build.ps1" }

# Certificado de prueba
$cer = Get-ChildItem $PackageDir -Filter *.cer | Select-Object -First 1
if ($cer) {
  certutil.exe -addstore -f Root $cer.FullName | Out-Null
  certutil.exe -addstore -f TrustedPublisher $cer.FullName | Out-Null
  Write-Host "Certificado $($cer.Name) importado."
} else {
  Write-Warning 'No hay .cer en el paquete; si el driver no esta firmado, la instalacion fallara.'
}

# Test signing
$bcd = bcdedit.exe /enum '{current}' | Out-String
if ($bcd -notmatch 'testsigning\s+Yes') {
  if ($EnableTestSigning) {
    bcdedit.exe /set testsigning on | Out-Null
    Write-Warning 'TESTSIGNING activado. Reinicia y vuelve a ejecutar este script.'
    return
  }
  Write-Warning 'TESTSIGNING esta desactivado. Ejecuta con -EnableTestSigning (y Secure Boot desactivado en la BIOS).'
  return
}

# Estabilidad: sin suspension
if (-not $KeepSleep) {
  powercfg.exe /change standby-timeout-ac 0
  powercfg.exe /change standby-timeout-dc 0
  powercfg.exe /hibernate off
  Write-Host 'Suspension e hibernacion desactivadas (el driver display-only no soporta S3/S4).'
}

pnputil.exe /add-driver $inf /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil fallo ($LASTEXITCODE)" }
Write-Host 'Driver instalado. Reinicia si el Administrador de dispositivos lo pide.' -ForegroundColor Green
