# blook-vmware-environment: produce the lab evidence set from real runs.
#
#   pwsh scripts/demo-e2e.ps1 [-Vm vm-evidence] [-Out <dir>]
#
# Requirements: a built bvmenv.exe, a base image (`bvmenv base build`) and the
# in-guest artifacts (`xmake f --guest_drivers=y`, then build
# guest-vtx-probe / guest-bsod-driver / guest-hang-driver).
#
# Every step has an external hard timeout so a wedged guest can never hang the
# script. No dry runs: every artifact comes from this host's VMware.
param(
    [string]$Vm = 'vm-evidence',
    [string]$Out = '',
    [switch]$SkipDeepVerify
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build\windows\x64\release'
if (-not (Test-Path (Join-Path $bin 'guest-bsod-driver.sys'))) { $bin = Join-Path $repo 'build\windows\x64\releasedbg' }
$exe = Join-Path $bin 'bvmenv.exe'
if (-not (Test-Path $exe)) { throw "bvmenv.exe not built: $exe (run: xmake f -y; xmake -y)" }
if (-not $Out) { $Out = Join-Path $repo ("docs\evidence\" + (Get-Date -Format 'yyyyMMdd') + '-lab-run') }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$env:BVMENV_VM_ROOT = if ($env:BVMENV_VM_ROOT) { $env:BVMENV_VM_ROOT } else { 'C:\blook-vms' }
$log = Join-Path $Out 'run.log'
Set-Content -LiteralPath $log -Value '' -Encoding UTF8

$summary = [ordered]@{
    schema     = 'bvmenv.evidence.v1'
    created_at = (Get-Date).ToString('o')
    host       = $env:COMPUTERNAME
    vm         = $Vm
    steps      = @()
}

function Step([string]$Message) {
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    Write-Host $line
    Add-Content -LiteralPath $log -Value $line
}

function Invoke-Bvmenv {
    param([string[]]$Arguments, [int]$TimeoutSeconds = 600)
    Step ('$ bvmenv ' + ($Arguments -join ' '))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $quoted = $Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }
    $psi.Arguments = ($quoted -join ' ')
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        try { $proc.Kill() } catch { }
        Step "  TIMEOUT after ${TimeoutSeconds}s; killed"
        return @{ Code = -1; Out = ''; Err = ''; TimedOut = $true }
    }
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    Step "  exit=$($proc.ExitCode)"
    Add-Content -LiteralPath $log -Value $stdout
    if ($stderr) { Add-Content -LiteralPath $log -Value $stderr }
    return @{ Code = $proc.ExitCode; Out = $stdout; Err = $stderr; TimedOut = $false }
}

function Timed([string]$Name, [string[]]$Arguments, [int]$TimeoutSeconds = 600) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $r = Invoke-Bvmenv -Arguments $Arguments -TimeoutSeconds $TimeoutSeconds
    $sw.Stop()
    $summary.steps += [ordered]@{
        name      = $Name
        args      = ($Arguments -join ' ')
        exit_code = $r.Code
        timed_out = $r.TimedOut
        seconds   = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    }
    return $r
}

function Save([string]$Name, $Result) {
    $path = Join-Path $Out $Name
    $Result.Out | Set-Content -LiteralPath $path -Encoding utf8
    return $path
}

# ---------------------------------------------------------------------------
Step '1. environment: doctor'
$r = Timed 'doctor' @('doctor', '--json') 120
$null = Save '00-doctor.json' $r

Step '2. base image contract: fast + deep'
$r = Timed 'base-verify-fast' @('base', 'verify', '--json') 120
$null = Save '01-base-verify-fast.json' $r
if ($SkipDeepVerify) {
    Step 'base verify --deep skipped (--SkipDeepVerify)'
} else {
    $r = Timed 'base-verify-deep' @('base', 'verify', '--deep', '--json') 1200
    $null = Save '02-base-verify-deep.json' $r
}

Step "3. linked clone $Vm from snapshot 'base'"
$r = Timed 'create' @('create', $Vm, '--json') 300
$null = Save '03-create.json' $r
$r = Timed 'wait-ready' @('wait-ready', $Vm, '--timeout', '240', '--json') 300
$null = Save '04-wait-ready.json' $r

Step '4. guest execution: stdout/stderr/exit code, timeout enforcement'
$r = Timed 'exec' @('exec', $Vm, '--timeout', '120', '--', 'cmd /c ver & whoami & echo BVMENV-EXEC-OK') 180
$null = Save '05-exec.txt' $r
$r = Timed 'exec-timeout' @('exec', $Vm, '--timeout', '5', '--json', '--', 'ping -n 40 127.0.0.1') 90
$null = Save '06-exec-timeout.json' $r

Step '5. push / pull'
$bundle = Join-Path $Out 'bin'
New-Item -ItemType Directory -Force -Path $bundle | Out-Null
foreach ($f in 'guest-nested-virt-check.exe', 'guest-bsod-driver.sys', 'guest-hang-driver.sys', 'guest-vtx-probe.sys') {
    $src = Join-Path $bin $f
    if (Test-Path $src) { Copy-Item $src $bundle -Force }
}
$r = Timed 'push' @('push', $Vm, $bundle, 'C:\blook\bin', '--json') 180
$null = Save '07-push.json' $r
$r = Timed 'pull' @('pull', $Vm, 'C:\blook\provision.json', (Join-Path $Out '08-guest-provision.json')) 120
$null = Save '09-pull.json' $r

Step '6. state on a healthy desktop'
$r = Timed 'state-desktop' @('state', $Vm, '--json', '--samples', '3', '--screenshot', (Join-Path $Out '10-desktop.png')) 180
$null = Save '10-state-desktop.json' $r

Step '7. state on a real logon screen (autologon disabled for one boot)'
$null = Timed 'disable-autologon' @('exec', $Vm, '--timeout', '60', '--', 'reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 0 /f') 120
$null = Timed 'stop' @('stop', $Vm) 120
$null = Timed 'start' @('start', $Vm, '--no-wait') 120
Start-Sleep -Seconds 45
$r = Timed 'state-login' @('state', $Vm, '--json', '--samples', '2', '--screenshot', (Join-Path $Out '11-login.png')) 180
$null = Save '11-state-login.json' $r
$null = Timed 'reset-after-login' @('reset', $Vm, '--json') 180
$null = Timed 'ready-after-login' @('wait-ready', $Vm, '--timeout', '180', '--json') 240

Step '8. nested virtualisation inside the guest (CPUID VMX + IA32_FEATURE_CONTROL)'
$nested = Join-Path $Out 'bundle-nested'
New-Item -ItemType Directory -Force -Path $nested | Out-Null
Copy-Item (Join-Path $bundle 'guest-nested-virt-check.exe') $nested -ErrorAction SilentlyContinue
$r = Timed 'e2e-nested-virt' @('e2e', $Vm, '--bundle', $nested, '--entry', 'guest-nested-virt-check.exe', '--timeout', '120', '--reset-on', 'never', '--out', (Join-Path $Out 'e2e')) 600
$null = Save '12-e2e-nested-virt.json' $r

Step '9. real BSOD (KeBugCheckEx in a test driver)'
$bsod = Join-Path $Out 'bundle-bsod'
New-Item -ItemType Directory -Force -Path $bsod | Out-Null
Copy-Item (Join-Path $bundle 'guest-bsod-driver.sys') $bsod -Force
@'
@echo off
copy /y "%~dp0guest-bsod-driver.sys" C:\Windows\System32\drivers\bvmenobsod.sys >nul
sc create bvmenobsod type= kernel binPath= C:\Windows\System32\drivers\bvmenobsod.sys >nul 2>&1
sc start bvmenobsod
exit /b %ERRORLEVEL%
'@ | Set-Content -LiteralPath (Join-Path $bsod 'run-bsod.cmd') -Encoding ASCII
$r = Timed 'e2e-bsod' @('e2e', $Vm, '--bundle', $bsod, '--entry', 'run-bsod.cmd', '--timeout', '45', '--reset-on', 'never', '--out', (Join-Path $Out 'e2e')) 900
$null = Save '13-e2e-bsod.json' $r

Step '10. reset after BSOD (target < 20 s)'
$r = Timed 'reset-after-bsod' @('reset', $Vm, '--json') 180
$null = Save '14-reset-after-bsod.json' $r
$r = Timed 'wait-ready-after-bsod' @('wait-ready', $Vm, '--timeout', '180', '--json') 240
$null = Save '15-ready-after-bsod.json' $r

Step '11. real hard hang (HIGH_LEVEL spin in a test driver)'
$hang = Join-Path $Out 'bundle-hang'
New-Item -ItemType Directory -Force -Path $hang | Out-Null
Copy-Item (Join-Path $bundle 'guest-hang-driver.sys') $hang -Force
@'
@echo off
copy /y "%~dp0guest-hang-driver.sys" C:\Windows\System32\drivers\bvmenohang.sys >nul
sc create bvmenohang type= kernel binPath= C:\Windows\System32\drivers\bvmenohang.sys >nul 2>&1
sc start bvmenohang
ping -n 10 127.0.0.1 >nul
exit /b 0
'@ | Set-Content -LiteralPath (Join-Path $hang 'run-hang.cmd') -Encoding ASCII
$r = Timed 'e2e-hang' @('e2e', $Vm, '--bundle', $hang, '--entry', 'run-hang.cmd', '--timeout', '40', '--reset-on', 'never', '--out', (Join-Path $Out 'e2e')) 900
$null = Save '16-e2e-hang.json' $r

Step '12. reset after hard hang (target < 20 s)'
$r = Timed 'reset-after-hang' @('reset', $Vm, '--json') 180
$null = Save '17-reset-after-hang.json' $r
$r = Timed 'wait-ready-after-hang' @('wait-ready', $Vm, '--timeout', '180', '--json') 240
$null = Save '18-ready-after-hang.json' $r

Step '13. final healthy state'
$r = Timed 'state-final' @('state', $Vm, '--json', '--samples', '3', '--screenshot', (Join-Path $Out '19-final.png')) 180
$null = Save '19-state-final.json' $r

$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Out 'summary.json') -Encoding utf8
Step "evidence written to $Out"
