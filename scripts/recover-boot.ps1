<#
.SYNOPSIS
  One-shot recovery from WinRE for the "BC250Kmd installed -> black screen" loop.

.DESCRIPTION
  Run this from the Windows Recovery Environment (Command Prompt -> powershell,
  or just `powershell -ExecutionPolicy Bypass -File C:\src\bc250\scripts\recover-boot.ps1`).
  It:
    1. Finds the offline Windows installation and loads its SYSTEM hive.
    2. Disables BC250Kmd, re-enables BC250Display and BasicDisplay.
    3. Removes the NOVGA token from SystemStartOptions.
    4. Empties any fake bc250umd.dll UserModeDriverName registration.
    5. Unloads the hive and removes novga / safeboot from the BCD store.

  After reboot the Microsoft Basic Display Adapter should give you a desktop.
  Then reinstall the display driver with .\scripts\install.ps1
#>
$ErrorActionPreference = 'Stop'
$roots = @('C:', 'D:', 'E:', 'F:', 'G:', 'H:')

$win = $null
foreach ($r in $roots) {
    if (Test-Path "$r\Windows\System32\config\SYSTEM") { $win = $r; break }
}
if (-not $win) { throw 'No offline Windows installation found. Check drive letters.' }
Write-Host "Windows installation: $win"

$hive = "$win\Windows\System32\config\SYSTEM"
$mount = 'HKLM\BC250RECOVER'

reg.exe unload $mount 2>$null | Out-Null   # clean up a previous failed run
reg.exe load $mount $hive | Out-Null
if ($LASTEXITCODE -ne 0) { throw "reg load failed ($LASTEXITCODE)" }

try {
    $sets = Get-ChildItem 'Registry::HKEY_LOCAL_MACHINE\BC250RECOVER' -ErrorAction Stop |
            Where-Object { $_.PSChildName -like 'ControlSet*' }
    foreach ($cs in $sets) {
        $p = $cs.Name
        Write-Host "Patching $($cs.PSChildName)"

        # 1. Kernel drivers
        foreach ($svc in @(@('BC250Kmd', 4), @('BC250Display', 3),
                           @('BasicDisplay', 1), @('BasicRender', 1))) {
            $key = Join-Path $p "Services\$($svc[0])"
            if (Test-Path $key) {
                reg.exe add $key /v Start /t REG_DWORD /d $svc[1] /f | Out-Null
            }
        }

        # 2. SystemStartOptions: NOVGA kills the Basic Display Adapter fallback
        $ctrl = Join-Path $p 'Control'
        if (Test-Path $ctrl) {
            $opts = (Get-ItemProperty $ctrl -Name SystemStartOptions -ErrorAction SilentlyContinue).SystemStartOptions
            if ($opts -and $opts -match 'NOVGA') {
                $new = ($opts -replace '(?i)NOVGA', '' -replace '\s+', ' ').Trim()
                reg.exe add $ctrl /v SystemStartOptions /t REG_SZ /d $new /f | Out-Null
                Write-Host "  SystemStartOptions: '$opts' -> '$new'"
            }
        }

        # 3. Fake UMD registration left by the experimental KMD INF
        $class = Join-Path $p 'Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
        Get-ChildItem $class -ErrorAction SilentlyContinue | ForEach-Object {
            foreach ($n in 'UserModeDriverName', 'UserModeDriverNameWow') {
                $v = (Get-ItemProperty $_.PSPath -Name $n -ErrorAction SilentlyContinue).$n
                if ($v -and (($v -join ' ') -match 'bc250umd')) {
                    Remove-ItemProperty -Path $_.PSPath -Name $n -ErrorAction SilentlyContinue
                    Write-Host "  removed $n from $($_.PSChildName)"
                }
            }
        }
    }
}
finally {
    reg.exe unload $mount | Out-Null
}

# 4. BCD: remove one-shot options that may keep the machine blind / in safe mode
$esp = $null
foreach ($r in @('C:', 'D:', 'E:', 'F:', 'G:', 'H:')) {
    if (Test-Path "$r\EFI\Microsoft\Boot\BCD") { $esp = $r; break }
}
if ($esp) {
    $store = "$esp\EFI\Microsoft\Boot\BCD"
    bcdedit.exe /store $store /deletevalue `{default`} novga 2>$null | Out-Null
    bcdedit.exe /store $store /deletevalue `{default`} safeboot 2>$null | Out-Null
    Write-Host "BCD cleaned: $store"
}
else {
    Write-Warning 'BCD store not found; you can run: bcdedit /deletevalue {default} novga'
}

Write-Host @'

Done. Reboot now. Windows should boot with the Microsoft Basic Display Adapter.
Once you have a desktop, restore the display driver:

    cd C:\src\bc250
    .\scripts\install.ps1

'@
