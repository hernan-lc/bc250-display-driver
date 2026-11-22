<#
.SYNOPSIS
  Boot guard used by install-kmd-safe.ps1. Runs once, ~90 s after a reboot.

.DESCRIPTION
  If the experimental BC250Kmd adapter failed to produce a display (DxgKrnl
  event 549 / device problem / black-screen live dump), it disables BC250Kmd,
  re-enables BC250Display, removes NOVGA and reboots. Otherwise it does nothing
  and removes itself. This turns a "black screen, pull the power cable"
  incident into an automatic one-shot recovery.
#>
$ErrorActionPreference = 'Continue'

Start-Sleep -Seconds 90

$failed = $false
$reasons = @()

try {
    $ev = Get-WinEvent -FilterHashtable @{
        LogName   = 'Microsoft-Windows-DxgKrnl-Admin'
        Id        = 549
        StartTime = (Get-Date).AddMinutes(-10)
    } -ErrorAction SilentlyContinue
    if ($ev) {
        $failed = $true
        $reasons += "DxgKrnl event 549 x$($ev.Count)"
    }
}
catch { }

try {
    $bad = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
           Where-Object { $_.Status -ne 'OK' -and $_.FriendlyName -match 'BC250|AMD' }
    if ($bad) {
        $failed = $true
        $reasons += ('Display device problem: ' + ($bad.FriendlyName -join ', '))
    }
}
catch { }

# A black-screen live dump written in the last 10 minutes is conclusive.
try {
    $dumps = Get-ChildItem "$env:SystemRoot\LiveKernelReports\WATCHDOG\WATCHDOG-*.dmp" -ErrorAction SilentlyContinue |
             Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-10) }
    if ($dumps) {
        $failed = $true
        $reasons += ('Live dump: ' + ($dumps.Name -join ', '))
    }
}
catch { }

if (-not $failed) {
    Write-EventLog -LogName Application -Source 'BC250' -EntryType Information -EventId 100 `
        -Message 'BC250 display guard: display OK, no revert needed.' -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName 'BC250-DisplayGuard' -Confirm:$false -ErrorAction SilentlyContinue
    return
}

$msg = 'BC250 display guard reverting KMD: ' + ($reasons -join '; ')
Write-EventLog -LogName Application -Source 'BC250' -EntryType Warning -EventId 101 `
    -Message $msg -ErrorAction SilentlyContinue

$base = 'HKLM:\SYSTEM\CurrentControlSet'
Set-ItemProperty "$base\Services\BC250Kmd" -Name Start -Value 4 -Type DWord -ErrorAction SilentlyContinue
Set-ItemProperty "$base\Services\BC250Display" -Name Start -Value 3 -Type DWord -ErrorAction SilentlyContinue

$opts = (Get-ItemProperty "$base\Control" -Name SystemStartOptions -ErrorAction SilentlyContinue).SystemStartOptions
if ($opts -and $opts -match 'NOVGA') {
    $new = ($opts -replace '(?i)NOVGA', '' -replace '\s+', ' ').Trim()
    Set-ItemProperty "$base\Control" -Name SystemStartOptions -Value $new -Type String -ErrorAction SilentlyContinue
}
bcdedit.exe /deletevalue `{current`} novga 2>$null | Out-Null

Unregister-ScheduledTask -TaskName 'BC250-DisplayGuard' -Confirm:$false -ErrorAction SilentlyContinue
Restart-Computer -Force
