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
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "No se encontro $msbuild" }

& $msbuild BC250Display.sln /m /p:Configuration=$Configuration /p:Platform=x64 /p:SignMode=TestSign
if ($LASTEXITCODE -ne 0) { throw 'La compilacion fallo' }

# 4) Juntar el paquete en out\
$out = Join-Path $root "out\$Configuration"
New-Item -ItemType Directory -Force $out | Out-Null
$pkg = Get-ChildItem -Recurse -Path "x64\$Configuration" -Filter BC250Display.inf | Select-Object -First 1
if ($pkg) {
  Copy-Item (Join-Path $pkg.DirectoryName '*') $out -Force
} else {
  Get-ChildItem -Recurse -Path "x64\$Configuration" -Include BC250Display.sys, BC250Display.cat, *.cer, BC250Display.inf |
    Copy-Item -Destination $out -Force
}
Write-Host "Paquete listo en $out" -ForegroundColor Green
Get-ChildItem $out | Format-Table Name, Length
