<#
.SYNOPSIS
  blook-vmware-environment: in-guest base image contract verification.

.DESCRIPTION
  Emitted as JSON on stdout so `bvmenv base verify` can assert machine
  readable results. Runs as SYSTEM (or any admin) inside the guest.

  Every check is "does the base image still satisfy the contract?" - see
  docs/base-image.md.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'SilentlyContinue'
$ProgressPreference = 'SilentlyContinue'

$checks = New-Object System.Collections.ArrayList

function Add-Check {
    param([string]$Name, [bool]$Ok, [string]$Detail = '')
    [void]$checks.Add([ordered]@{ name = $Name; ok = $Ok; detail = $Detail })
}

function Test-Reg {
    param([string]$Path, [string]$Name, [object]$Expected)
    $value = (Get-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction SilentlyContinue).$Name
    if ($null -eq $value) { return $false }
    return ([string]$value -eq [string]$Expected)
}

# --- bcdedit / test signing / kernel debug --------------------------------
$bcd = (& bcdedit.exe /enum '{current}' 2>&1) -join "`n"
Add-Check 'testsigning off (DSE enforced)' (-not ($bcd -match 'testsigning\s+Yes')) 'drivers must be SPC-signed'
Add-Check 'bcdedit nointegritychecks on' ($bcd -match 'nointegritychecks\s+Yes') 'advapi no integrity checks'
Add-Check 'kernel debugging on' ($bcd -match 'debug\s+Yes') 'bcdedit /debug on'
$dbg = (& bcdedit.exe /dbgsettings 2>&1) -join "`n"
Add-Check 'kernel debug over serial' (($dbg -match 'serial') -and ($dbg -match '115200')) 'bcdedit /dbgsettings: serial 115200'
Add-Check 'hypervisorlaunchtype off' ($bcd -match 'hypervisorlaunchtype\s+Off') 'no Hyper-V: nested VT-x is free for the lab hypervisor'

# --- bugcheck behaviour ---------------------------------------------------
$crash = 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
Add-Check 'bugcheck: no auto reboot' (Test-Reg $crash 'AutoReboot' 0) 'AutoReboot=0 keeps the BSOD on screen'
Add-Check 'bugcheck: display parameters' (Test-Reg $crash 'DisplayParameters' 1) 'stop code visible on screen'
Add-Check 'ctrl+scroll crash enabled' (Test-Reg 'HKLM:\SYSTEM\CurrentControlSet\Services\i8042prt\Parameters' 'CrashOnCtrlScroll' 1) 'bvmenv crash'

# --- virtualisation based security must be off ---------------------------
$dg = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard'
Add-Check 'VBS off' (Test-Reg $dg 'EnableVirtualizationBasedSecurity' 0) 'no VBS inside the guest'
Add-Check 'HVCI off' (Test-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' 'Enabled' 0) 'memory integrity off'

# --- usability ------------------------------------------------------------
Add-Check 'UAC disabled' (Test-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' 'EnableLUA' 0) 'elevated guest ops by default'
Add-Check 'fast startup off' (Test-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' 'HiberbootEnabled' 0) 'clean shutdown semantics'
$tools = Get-Service -Name 'VMTools' -ErrorAction SilentlyContinue
Add-Check 'vmware tools running' ($tools -and $tools.Status -eq 'Running') 'guest ops need vmtoolsd'
Add-Check 'dev account exists' ([bool](Get-LocalUser -Name 'blook' -ErrorAction SilentlyContinue)) 'blook / blook'
$rootStore = (& certutil.exe -store Root 2>&1) -join "`n"
Add-Check 'SPC trust: tsa root in Root store' ($rootStore -match 'tsa') 'blook-drv signer tsa-root.cer'
$crash = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' -ErrorAction SilentlyContinue
$dumpOk = ($crash.CrashDumpEnabled -eq 1) -and ($crash.DumpFile -match 'MEMORY\.DMP')
Add-Check 'crash dump configured (kernel dump)' $dumpOk ('CrashDumpEnabled=' + $crash.CrashDumpEnabled + ' DumpFile=' + $crash.DumpFile)
Add-Check 'lock screen disabled' (Test-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization' 'NoLockScreen' 1) 'headless screenshots stay on the desktop'

# --- nested virtualisation (the lab hypervisor needs VT-x) ---------------
$nested = $false
$detail = 'nested-virt-check.exe not present'
if (Test-Path -LiteralPath 'C:\blook\nested-virt-check.exe') {
    $out = (& 'C:\blook\nested-virt-check.exe' 2>&1) -join "`n"
    $nested = ($out -match '(?m)^VMX\s*=\s*1')
    $detail = ($out -replace "`r?`n", ' ').Trim()
}
Add-Check 'VT-x visible in guest (CPUID VMX)' $nested $detail

# --- nested virtualisation ground truth (ring 0 probe from provisioning) ---
$vtxOk = $false
$vtxDetail = 'provision.json not readable'
$provisionRaw = Get-Content -LiteralPath 'C:\blook\provision.json' -Raw -ErrorAction SilentlyContinue
if ($provisionRaw) {
    try {
        $provision = $provisionRaw | ConvertFrom-Json
        $vtx = $provision.nested_virt
        if ($vtx) {
            $vtxOk = [bool]$vtx.ok
            $vtxDetail = ("cpuid_vmx={0} lock={1} vmxon_outside_smx={2} feature_control={3} vmx_basic={4}" -f `
                    $vtx.cpuid_vmx, $vtx.feature_control_locked, $vtx.vmxon_outside_smx, `
                    $vtx.feature_control_low, $vtx.vmx_basic_low)
        }
    } catch { $vtxDetail = "parse error: $($_.Exception.Message)" }
}
Add-Check 'ring-0 VT-x probe (IA32_FEATURE_CONTROL)' $vtxOk $vtxDetail

# --- provisioning ---------------------------------------------------------
$provision = Get-Content -LiteralPath 'C:\blook\provision.json' -Raw
$ok = $false
if ($provision) {
    try {
        $json = $provision | ConvertFrom-Json
        $ok = ($json.schema -eq 'bvmenv.provision.v1')
    } catch { $ok = $false }
}
Add-Check 'provisioning completed' $ok 'C:\blook\provision.json'

$result = [ordered]@{
    schema        = 'bvmenv.guest-verify.v1'
    computer_name = $env:COMPUTERNAME
    os_version    = [System.Environment]::OSVersion.Version.ToString()
    ok            = -not ($checks | Where-Object { -not $_.ok })
    checks        = $checks
}
$result | ConvertTo-Json -Depth 5
