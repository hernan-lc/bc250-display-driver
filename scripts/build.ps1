<#
.SYNOPSIS
  Builds BC250Display.sys (and the user-mode tools) using the WDK from NuGet.
.REQUIREMENTS
  Visual Studio 2022 with "Desktop development with C++", the
  "MSVC v143 - Spectre-mitigated libs (x64/x86)" component and the
  "Windows Driver Kit" component (VS extension). Headers/tools come from NuGet.
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
  Write-Host 'Downloading nuget.exe from dist.nuget.org...'
  Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nuget
}

# 2) WDK + SDK from NuGet
& $nuget restore packages.config -PackagesDirectory packages
if ($LASTEXITCODE -ne 0) { throw 'nuget restore failed' }

# 3) MSBuild
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) {
  throw ('The C++ compiler (MSVC) is missing. In the Visual Studio Installer add: ' +
         '"Desktop development with C++" + "MSVC v143 Spectre-mitigated libs (x64/x86)" + "Windows Driver Kit". ' +
         'Alternative: use the GitHub Actions workflow (.github/workflows/build.yml).')
}
# 64-bit MSBuild: the x64 NuGet WDK only ships x64 tools (InfVerif, ApiValidator)
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "$msbuild not found" }

& $msbuild BC250Display.sln /m /p:Configuration=$Configuration /p:Platform=x64 /p:SignMode=TestSign
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

# 4) Installable packages: out\<Config>\ (BC250Display, default) and out\<Config>\kmd\ (BC250Kmd, experimental)
$out = Join-Path $root "out\$Configuration"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out, (Join-Path $out 'kmd') | Out-Null
$bin = "x64\$Configuration"
Get-ChildItem $bin -Filter 'bc250*.exe' -ErrorAction SilentlyContinue | Copy-Item -Destination $out -Force

$wdkBin = Join-Path $root 'packages\Microsoft.Windows.WDK.x64.10.0.26100.6584\c\bin\10.0.26100.0'
$sdkBin = Join-Path $root 'packages\Microsoft.Windows.SDK.CPP.10.0.26100.6584\c\bin\10.0.26100.0\x64'
$inf2cat = Join-Path $wdkBin 'x86\Inf2Cat.exe'
$signtool = Join-Path $sdkBin 'signtool.exe'

# Temporary test certificate shared by both packages, private key deleted at the end.
# Order per package: sign .sys -> generate .cat (stores the .sys hash) -> sign .cat
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=BC250Display Test' `
          -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
function New-DriverPackage([string]$Name, [string]$SysDir, [string]$InfName, [string]$Dest) {
  Copy-Item "$SysDir\$Name.sys", "$SysDir\$Name.pdb" $Dest -Force
  $infSrc = Get-ChildItem -Recurse $SysDir -Filter $InfName | Select-Object -First 1
  if (-not $infSrc) { $infSrc = Get-Item (Join-Path $root "package\$InfName") }
  Copy-Item $infSrc.FullName (Join-Path $Dest "$Name.inf") -Force
  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$Dest\$Name.sys"
  if ($LASTEXITCODE -ne 0) { throw "signtool ($Name.sys) failed" }
  # Catalog for Windows 10 1709+ and Windows 11 (the INFs require 10.0.16299+)
  & $inf2cat "/driver:$Dest" '/os:10_RS3_X64,10_RS4_X64,10_RS5_X64,10_19H1_X64,10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64' /uselocaltime
  if ($LASTEXITCODE -ne 0) { throw "Inf2Cat ($Name) failed" }
  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$Dest\$Name.cat"
  if ($LASTEXITCODE -ne 0) { throw "signtool ($Name.cat) failed" }
  Export-Certificate -Cert $cert -FilePath "$Dest\BC250Display.cer" | Out-Null
}
try {
  New-DriverPackage 'BC250Display' $bin 'bc250display.inf' $out
  New-DriverPackage 'BC250Kmd' "$bin\kmd" 'bc250kmd.inf' (Join-Path $out 'kmd')
} finally {
  Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue
}

Write-Host "Packages ready in $out (BC250Display) and $out\kmd (BC250Kmd, experimental)" -ForegroundColor Green
Get-ChildItem $out -Recurse -File | Format-Table @{n='File';e={$_.FullName.Substring($out.Length+1)}}, Length