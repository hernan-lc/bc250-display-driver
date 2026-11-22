<#
.SYNOPSIS
  Compila BC250Display.sys usando el WDK por NuGet (no hace falta instalar el WDK).
.REQUIREMENTS
  Visual Studio 2022 con la carga "Desarrollo para el escritorio con C++"
  y los componentes "MSVC v143 - bibliotecas con mitigacion de Spectre (x64)".
#>
param(
  [ValidateSet('Debug','Release')] [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# 1) nuget.exe
$tools = Join-Path $root 'tools'
$nuget = Join-Path $tools 'nuget.exe'
if (-not (Test-Path $nuget)) {
  New-Item -ItemType Directory -Force $tools | Out-Null
  Write-Host 'Descargando nuget.exe desde dist.nuget.org...'
  Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nuget
}

# 2) WDK + SDK desde NuGet
& $nuget restore packages.config -PackagesDirectory packages
if ($LASTEXITCODE -ne 0) { throw 'nuget restore fallo' }

# 3) MSBuild
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) {
  throw ('Falta el compilador C++ (MSVC). En el Visual Studio Installer instala: ' +
         '"Desarrollo para el escritorio con C++" + "MSVC v143 bibliotecas con mitigacion de Spectre (x64/x86)". ' +
         'Alternativa: usa el workflow de GitHub Actions (.github/workflows/build.yml).')
}
# 64-bit MSBuild: the x64 NuGet WDK only ships x64 tools (InfVerif, ApiValidator)
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "No se encontro $msbuild" }

& $msbuild BC250Display.sln /m /p:Configuration=$Configuration /p:Platform=x64 /p:SignMode=TestSign
if ($LASTEXITCODE -ne 0) { throw 'La compilacion fallo' }

# 4) Build the installable package in out\<Config>: .sys + .inf + signed .cat + .cer
$out = Join-Path $root "out\$Configuration"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$bin = "x64\$Configuration"
Copy-Item "$bin\BC250Display.sys", "$bin\BC250Display.pdb" $out -Force
Copy-Item (Get-ChildItem -Recurse $bin -Filter bc250display.inf | Select-Object -First 1).FullName (Join-Path $out 'BC250Display.inf') -Force

$wdkBin = Join-Path $root 'packages\Microsoft.Windows.WDK.x64.10.0.26100.6584\c\bin\10.0.26100.0'
$sdkBin = Join-Path $root 'packages\Microsoft.Windows.SDK.CPP.10.0.26100.6584\c\bin\10.0.26100.0\x64'
$inf2cat = Join-Path $wdkBin 'x86\Inf2Cat.exe'
$signtool = Join-Path $sdkBin 'signtool.exe'

# Temporary test certificate. Order: sign .sys -> generate .cat (stores the .sys hash) -> sign .cat
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=BC250Display Test' `
          -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
try {
  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$out\BC250Display.sys"
  if ($LASTEXITCODE -ne 0) { throw 'signtool (.sys) fallo' }

  # Catalog for Windows 10 1709+ and Windows 11 (the INF requires 10.0.16299+)
  & $inf2cat "/driver:$out" '/os:10_RS3_X64,10_RS4_X64,10_RS5_X64,10_19H1_X64,10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64' /uselocaltime
  if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat fallo' }

  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$out\BC250Display.cat"
  if ($LASTEXITCODE -ne 0) { throw 'signtool (.cat) fallo' }
  Export-Certificate -Cert $cert -FilePath "$out\BC250Display.cer" | Out-Null
} finally {
  Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue
}

Write-Host "Paquete listo en $out" -ForegroundColor Green
Get-ChildItem $out | Format-Table Name, Length
