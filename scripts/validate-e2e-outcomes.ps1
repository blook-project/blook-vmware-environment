<#
.SYNOPSIS
Validates the bvmenv e2e failure-classification contract on a real fork:
timeout, BSOD (with auto-reset) and hard hang (with auto-reset).

.DESCRIPTION
One-shot, self-cleaning: creates a linked clone, boots it, runs three e2e
scenarios against real guest behaviour, asserts the bvmenv.e2e.v1 outcomes, then
deletes the fork. Used to close the "e2e classifies bsod/hang + resets" gap and
re-runnable by any owner.

.EXAMPLE
pwsh -File scripts/validate-e2e-outcomes.ps1 -Vm vm-e2e-check
#>
param(
    [string]$Vm = 'vm-e2e-check',
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\windows\x64\releasedbg\bvmenv.exe'),
    [string]$Out = (Join-Path $PSScriptRoot '..\docs\evidence\e2e-outcomes'),
    [int]$ReadyTimeout = 600
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "bvmenv not found: $Exe" }
$exe = (Resolve-Path $Exe).Path
$build = Split-Path $exe -Parent
$results = @{ vm = $Vm; started_at = (Get-Date).ToUniversalTime().ToString('o'); cases = @() }

function Invoke-Bvmenv {
    param([string[]]$Argv)
    $out = & $exe @Argv 2>&1
    $obj = $null
    try { $obj = ($out | Out-String) | ConvertFrom-Json -ErrorAction Stop } catch { }
    return @{ raw = ($out | Out-String); json = $obj }
}

New-Item -ItemType Directory -Force $Out | Out-Null
$bundles = Join-Path $Out 'bundles'
New-Item -ItemType Directory -Force $bundles | Out-Null

# --- bundle: a command that ignores its budget -------------------------------
$slow = Join-Path $bundles 'slow'
New-Item -ItemType Directory -Force $slow | Out-Null
Set-Content (Join-Path $slow 'run.cmd') -Encoding ASCII -Value @'
@echo off
%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -NonInteractive -Command "Start-Sleep -Seconds 40"
exit /b 0
'@

# --- bundle: real bugcheck ---------------------------------------------------
$bsod = Join-Path $bundles 'bsod'
New-Item -ItemType Directory -Force $bsod | Out-Null
Copy-Item (Join-Path $build 'guest-bsod-driver.sys') $bsod -Force
Set-Content (Join-Path $bsod 'run-bsod.cmd') -Encoding ASCII -Value @'
@echo off
copy /y "%~dp0guest-bsod-driver.sys" C:\Windows\System32\drivers\bvmenobsod.sys >nul
sc create bvmenobsod type= kernel binPath= C:\Windows\System32\drivers\bvmenobsod.sys >nul 2>&1
sc start bvmenobsod
exit /b %ERRORLEVEL%
'@

# --- bundle: real hard hang --------------------------------------------------
$hang = Join-Path $bundles 'hang'
New-Item -ItemType Directory -Force $hang | Out-Null
Copy-Item (Join-Path $build 'guest-hang-driver.sys') $hang -Force
Set-Content (Join-Path $hang 'run-hang.cmd') -Encoding ASCII -Value @'
@echo off
copy /y "%~dp0guest-hang-driver.sys" C:\Windows\System32\drivers\bvmenohang.sys >nul
sc create bvmenohang type= kernel binPath= C:\Windows\System32\drivers\bvmenohang.sys >nul 2>&1
sc start bvmenohang
exit /b 0
'@

function Wait-Ready {
    & $exe wait-ready $Vm --timeout $ReadyTimeout 2>&1 | Out-Null
}

function Add-Case {
    param([string]$Name, [string]$ExpectedOutcome, [bool]$ExpectReset, [int]$TimeoutSec, [string]$Bundle, [string]$Entry)
    $runs = Join-Path $Out 'runs'
    $r = Invoke-Bvmenv @('e2e', $Vm, '--bundle', $Bundle, '--entry', $Entry, '--timeout', "$TimeoutSec", '--out', $runs, '--json')
    $result = $r.json
    $actual = $result.outcome
    $resetOk = (-not $ExpectReset) -or ($result.reset.performed -eq $true)
    $ok = ($actual -eq $ExpectedOutcome) -and $resetOk
    $case = [ordered]@{
        case              = $Name
        expected_outcome  = $ExpectedOutcome
        actual_outcome    = $actual
        timed_out         = $result.timed_out
        stop_code         = $result.state_after.screen.stop_code
        classification    = $result.state_after.screen.classification
        reset_performed   = $result.reset.performed
        reset_strategy    = $result.reset.strategy
        artifact_dir      = $result.artifact_dir
        ok                = $ok
    }
    $script:results.cases += $case
    Write-Host ("[e2e] {0,-8} expected={1,-8} actual={2,-8} reset={3} ok={4}" -f `
        $Name, $ExpectedOutcome, $actual, $result.reset.performed, $ok)
    return $ok
}

try {
    & $exe delete $Vm --force 2>&1 | Out-Null
    & $exe create $Vm --no-start 2>&1 | Out-Null
    & $exe start $Vm --no-wait 2>&1 | Out-Null
    Wait-Ready

    $allOk = $true
    # timeout: never reset (the guest is healthy, we keep it for the next case)
    $allOk = (Add-Case 'timeout' 'timeout' $false 5 $slow 'run.cmd') -and $allOk

    # bsod: default --reset-on failure must reset the fork
    $allOk = (Add-Case 'bsod' 'bsod' $true 40 $bsod 'run-bsod.cmd') -and $allOk

    # hang: restart, then expect hang + reset
    & $exe start $Vm --no-wait 2>&1 | Out-Null
    Wait-Ready
    $allOk = (Add-Case 'hang' 'hang' $true 60 $hang 'run-hang.cmd') -and $allOk

    $results.all_ok = $allOk
}
finally {
    $results.finished_at = (Get-Date).ToUniversalTime().ToString('o')
    ($results | ConvertTo-Json -Depth 6) | Set-Content (Join-Path $Out 'summary.json') -Encoding utf8
    & $exe stop $Vm --hard 2>&1 | Out-Null
    & $exe delete $Vm --force 2>&1 | Out-Null
    Write-Host "artifacts: $Out"
}

if (-not $results.all_ok) { exit 1 }
exit 0
