# blook-vmware-environment: produce the lab evidence set.
#
#   pwsh scripts/demo-e2e.ps1 [-Vm vm-drv] [-Out docs/evidence/20261021-lab-run]
#
# Requires: a built `bvmenv.exe`, a base image (`bvmenv base build`) and the
# in-guest test drivers (`xmake f --guest_drivers=y; xmake -y guest-bsod-driver;
# xmake -y guest-hang-driver`).
#
# The script exercises, and records evidence for:
#   * base image contract (base verify)
#   * linked-clone create + exec + push/pull
#   * nested virtualisation inside the guest (CPUID VMX)
#   * a real bugcheck (KeBugCheckEx in a test driver) -> state classifies bsod
#   * a real hard hang (HIGH_LEVEL spin in a test driver) -> state classifies hang
#   * reset recovery after both
param(
    [string]$Vm = 'vm-drv',
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\windows\x64\releasedbg\bvmenv.exe'
if (-not (Test-Path $exe)) { throw "bvmenv.exe not built: $exe (run: xmake f -y; xmake -y)" }
if (-not $Out) { $Out = Join-Path $repo ("docs\evidence\" + (Get-Date -Format 'yyyyMMdd') + '-lab-run') }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$env:BVMENV_VM_ROOT = if ($env:BVMENV_VM_ROOT) { $env:BVMENV_VM_ROOT } else { 'C:\blook-vms' }
$bvmenv = { param([string[]]$Arguments) & $exe @Arguments }
$log = Join-Path $Out 'run.log'
function Step([string]$Message) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $Message
    Write-Host $line
    Add-Content -LiteralPath $log -Value $line
}
function Run([string[]]$Arguments) {
    Step ("bvmenv " + ($Arguments -join ' '))
    $out = & $exe @Arguments 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Add-Content -LiteralPath $log -Value $_ }
    return @{ Output = ($out -join "`n"); Code = $code }
}

$summary = [ordered]@{
    schema     = 'bvmenv.evidence.v1'
    created_at = (Get-Date).ToString('o')
    vm         = $Vm
    steps      = @()
}
function Add-Step([string]$Name, [hashtable]$Data) {
    $summary.steps += [ordered]@{ name = $Name; data = $Data }
}

# ---------------------------------------------------------------------------
Step '1. base image contract'
$verify = Run @('base', 'verify', '--json')
$verify | Out-File -Encoding utf8 (Join-Path $Out '01-base-verify.json')
Copy-Item (Join-Path $Out '01-base-verify.json') (Join-Path $Out '01-base-verify.txt') -Force
Add-Step 'base verify' @{ exit_code = $verify.Code; artifact = '01-base-verify.json' }
if ($verify.Code -ne 0) { Step 'WARNING: base verify failed, continuing for evidence' }

# ---------------------------------------------------------------------------
Step "2. create linked clone $Vm"
$reset = Run @('reset', $Vm, '--json')
if ($reset.Code -ne 0) {
    $create = Run @('create', $Vm, '--json')
    $create | Out-File -Encoding utf8 (Join-Path $Out '02-create.json')
    Add-Step 'create' @{ exit_code = $create.Code; artifact = '02-create.json' }
} else {
    $reset | Out-File -Encoding utf8 (Join-Path $Out '02-reset.json')
    Add-Step 'reset (existing fork)' @{ exit_code = $reset.Code; artifact = '02-reset.json' }
}

Step '3. exec + push/pull'
$exec = Run @('exec', $Vm, '--timeout', '120', '--', 'cmd /c ver & whoami & echo BVMENV-EXEC-OK')
$exec.Output | Out-File -Encoding utf8 (Join-Path $Out '03-exec.txt')
Add-Step 'exec' @{ exit_code = $exec.Code; artifact = '03-exec.txt' }

$binDir = Join-Path $Out 'bin'
New-Item -ItemType Directory -Force -Path $binDir | Out-Null
Copy-Item (Join-Path $repo 'build\windows\x64\releasedbg\guest-nested-virt-check.exe') $binDir -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repo 'build\windows\x64\releasedbg\guest-bsod-driver.sys') $binDir -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repo 'build\windows\x64\releasedbg\guest-hang-driver.sys') $binDir -ErrorAction SilentlyContinue
$push = Run @('push', $Vm, $binDir, 'C:\blook\bin')
Add-Step 'push drivers' @{ exit_code = $push.Code }
$pull = Run @('pull', $Vm, 'C:\blook\provision.json', (Join-Path $Out '04-guest-provision.json'))
Add-Step 'pull provision.json' @{ exit_code = $pull.Code; artifact = '04-guest-provision.json' }

# ---------------------------------------------------------------------------
Step '4. nested virtualisation check inside the guest'
$nestedBundle = Join-Path $Out 'bundle-nested'
New-Item -ItemType Directory -Force -Path $nestedBundle | Out-Null
Copy-Item (Join-Path $binDir 'guest-nested-virt-check.exe') $nestedBundle
$e2eNested = Run @('e2e', $Vm, '--bundle', $nestedBundle, '--entry', 'guest-nested-virt-check.exe',
    '--timeout', '120', '--reset-on', 'never', '--out', $Out)
$e2eNested.Output | Out-File -Encoding utf8 (Join-Path $Out '05-e2e-nested-virt.json')
Add-Step 'e2e nested virt' @{ exit_code = $e2eNested.Code; artifact = '05-e2e-nested-virt.json' }

# ---------------------------------------------------------------------------
Step '5. BSOD scenario (KeBugCheckEx in a test driver)'
$bsodBundle = Join-Path $Out 'bundle-bsod'
New-Item -ItemType Directory -Force -Path $bsodBundle | Out-Null
Copy-Item (Join-Path $binDir 'guest-bsod-driver.sys') $bsodBundle
@'
@echo off
rem Register + start the bugcheck driver; the guest will bugcheck immediately.
sc create bvmenobsod type= kernel binPath= C:\Windows\System32\drivers\guest-bsod-driver.sys >nul 2>&1
sc start bvmenobsod
exit /b %ERRORLEVEL%
'@ | Set-Content -LiteralPath (Join-Path $bsodBundle 'run-bsod.cmd') -Encoding ASCII
$e2eBsod = Run @('e2e', $Vm, '--bundle', $bsodBundle, '--entry', 'run-bsod.cmd',
    '--timeout', '180', '--reset-on', 'failure', '--out', $Out)
$e2eBsod.Output | Out-File -Encoding utf8 (Join-Path $Out '06-e2e-bsod.json')
Add-Step 'e2e bsod' @{ exit_code = $e2eBsod.Code; artifact = '06-e2e-bsod.json' }

Step '6. reset after BSOD'
$resetBsod = Run @('reset', $Vm, '--json')
$resetBsod | Out-File -Encoding utf8 (Join-Path $Out '07-reset-after-bsod.json')
Add-Step 'reset after bsod' @{ exit_code = $resetBsod.Code; artifact = '07-reset-after-bsod.json' }
$readyAfterBsod = Run @('wait-ready', $Vm, '--timeout', '300', '--json')
$readyAfterBsod | Out-File -Encoding utf8 (Join-Path $Out '08-ready-after-bsod.json')

# ---------------------------------------------------------------------------
Step '7. hard hang scenario (HIGH_LEVEL spin in a test driver)'
$hangBundle = Join-Path $Out 'bundle-hang'
New-Item -ItemType Directory -Force -Path $hangBundle | Out-Null
Copy-Item (Join-Path $binDir 'guest-hang-driver.sys') $hangBundle
@'
@echo off
rem Register + start the hang driver: the guest stops scheduling ~3 s later.
sc create bvmenohang type= kernel binPath= C:\Windows\System32\drivers\guest-hang-driver.sys >nul 2>&1
sc start bvmenohang
rem stay alive briefly so the harness can sample the state after the hang
ping -n 8 127.0.0.1 >nul
exit /b 0
'@ | Set-Content -LiteralPath (Join-Path $hangBundle 'run-hang.cmd') -Encoding ASCII
$e2eHang = Run @('e2e', $Vm, '--bundle', $hangBundle, '--entry', 'run-hang.cmd',
    '--timeout', '120', '--reset-on', 'failure', '--out', $Out)
$e2eHang.Output | Out-File -Encoding utf8 (Join-Path $Out '09-e2e-hang.json')
Add-Step 'e2e hang' @{ exit_code = $e2eHang.Code; artifact = '09-e2e-hang.json' }

Step '8. reset after hang'
$resetHang = Run @('reset', $Vm, '--json')
$resetHang | Out-File -Encoding utf8 (Join-Path $Out '10-reset-after-hang.json')
$readyAfterHang = Run @('wait-ready', $Vm, '--timeout', '300', '--json')
$readyAfterHang | Out-File -Encoding utf8 (Join-Path $Out '11-ready-after-hang.json')
Add-Step 'reset after hang' @{ exit_code = $resetHang.Code; artifact = '10-reset-after-hang.json' }

# ---------------------------------------------------------------------------
Step '9. final state'
$state = Run @('state', $Vm, '--json', '--samples', '3')
$state.Output | Out-File -Encoding utf8 (Join-Path $Out '12-final-state.json')
Add-Step 'final state' @{ exit_code = $state.Code; artifact = '12-final-state.json' }

$summary | ConvertTo-Json -Depth 6 | Out-File -Encoding utf8 (Join-Path $Out 'summary.json')
Step "evidence written to $Out"
