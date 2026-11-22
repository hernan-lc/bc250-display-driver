<#
.SYNOPSIS
  Downloads the Cyan Skillfish GPU firmware needed by M4+ from the official linux-firmware repository.
.DESCRIPTION
  Files land in firmware\ (not committed). AMD's redistribution license (LICENSE.amdgpu) is
  downloaded next to them and must be kept with the files.
#>
param([string]$Dest = (Join-Path (Split-Path -Parent $PSScriptRoot) 'firmware'))
$ErrorActionPreference = 'Stop'
$base = 'https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main'
$files = @(
  'LICENSE.amdgpu',
  'amdgpu/cyan_skillfish2_sdma.bin',
  'amdgpu/cyan_skillfish2_sdma1.bin',
  # needed for the future CP/RLC bring-up (graphics/compute queues)
  'amdgpu/cyan_skillfish2_rlc.bin',
  'amdgpu/cyan_skillfish2_mec.bin',
  'amdgpu/cyan_skillfish2_me.bin',
  'amdgpu/cyan_skillfish2_pfp.bin',
  'amdgpu/cyan_skillfish2_ce.bin'
)
New-Item -ItemType Directory -Force $Dest | Out-Null
foreach ($f in $files) {
  $out = Join-Path $Dest (Split-Path $f -Leaf)
  Write-Host "Downloading $f"
  Invoke-WebRequest "$base/$f" -OutFile $out -UseBasicParsing
}
Get-ChildItem $Dest | Format-Table Name, Length
