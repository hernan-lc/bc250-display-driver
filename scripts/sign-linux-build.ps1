<#
.SYNOPSIS
  Windows side of the Linux workflow: turns the Linux-built (unsigned) BC250Kmd.sys into an installable
  test-signed package in out\<Config>\kmd, ready for scripts\install-kmd-safe.ps1.

.DESCRIPTION
  scripts/linux/build.sh  -> clang-cl/lld-link build on Linux
  scripts/linux/deploy.sh -> copies it to C:\src\bc250\out\linux-<Config>\kmd
  THIS SCRIPT             -> stampinf + sign .sys + Inf2Cat + sign .cat  (same steps as build.ps1 step 4)
  install-kmd-safe.ps1    -> installs with restore point + boot guard

  UNTESTED on hardware as of writing: review once before first use.
#>
param([ValidateSet('Release','Debug')] [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root "out\linux-$Configuration\kmd"
$dest = Join-Path $root "out\$Configuration\kmd"
if (-not (Test-Path (Join-Path $src 'BC250Kmd.sys'))) { throw "$src\BC250Kmd.sys missing: run scripts/linux/deploy.sh on Linux first" }

$wdkBin  = Join-Path $root 'packages\Microsoft.Windows.WDK.x64.10.0.26100.6584\c\bin\10.0.26100.0'
$sdkBin  = Join-Path $root 'packages\Microsoft.Windows.SDK.CPP.10.0.26100.6584\c\bin\10.0.26100.0\x64'
$inf2cat = Join-Path $wdkBin 'x86\Inf2Cat.exe'
$stamp   = Join-Path $wdkBin 'x64\stampinf.exe'
$signtool = Join-Path $sdkBin 'signtool.exe'

if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$src\BC250Kmd.sys", "$src\BC250Kmd.pdb", "$src\BC250Kmd.inf" $dest -Force

# Stamp DriverVer (timestamp-based so every build is a new, higher version)
$ver = '{0}.{1}.{2}.{3}' -f 5, (Get-Date).DayOfYear, (Get-Date).Hour, (Get-Date).Minute
& $stamp -f "$dest\BC250Kmd.inf" -a amd64 -d '*' -v $ver -k 1.15 -u 2.33.0
if ($LASTEXITCODE -ne 0) { throw 'stampinf failed' }

$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=BC250Display Test' `
          -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
try {
  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$dest\BC250Kmd.sys";  if ($LASTEXITCODE) { throw 'sign .sys failed' }
  & $inf2cat "/driver:$dest" '/os:10_RS3_X64,10_RS4_X64,10_RS5_X64,10_19H1_X64,10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64' /uselocaltime
  if ($LASTEXITCODE) { throw 'Inf2Cat failed' }
  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$dest\BC250Kmd.cat";  if ($LASTEXITCODE) { throw 'sign .cat failed' }
  Export-Certificate -Cert $cert -FilePath "$dest\BC250Display.cer" | Out-Null
} finally {
  Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue
}
Write-Host "Package ready: $dest   (next: .\scripts\install-kmd-safe.ps1)" -ForegroundColor Green
Get-ChildItem $dest | Format-Table Name, Length
