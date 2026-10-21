<#
.SYNOPSIS
  blook-vmware-environment guest provisioning (runs as SYSTEM, idempotent).

.DESCRIPTION
  Runs from the "blook-provision" boot scheduled task created by stage.cmd.
  Phase 1: wait for a usable desktop, install VMware Tools, apply the test-VM
           baseline (test signing, kernel debug, no auto-reboot on bugcheck,
           UAC off, VBS/HVCI off, no sleep/hibernate, Windows Update and
           Defender realtime disabled, no lock screen), then reboot.
  Phase 2: verify the baseline, write C:\blook\provision.json, remove the
           task and shut the VM down cleanly so the host can snapshot "base".

  Every step is best effort: failures are logged and do not abort the run,
  because the host-side base build validates the result afterwards.
#>
[CmdletBinding()]
param(
    [string]$LogFile = 'C:\blook\provision.log', [switch]$Repair
)

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'
$script:Failures       = @()

function Log {
    param([string]$Message)
    $line = '{0} {1}' -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), $Message
    Write-Output $line
    try { Add-Content -LiteralPath $LogFile -Value $line -Encoding UTF8 } catch { }
}

function Invoke-Step {
    param([string]$Name, [scriptblock]$Body)
    try {
        Log "BEGIN $Name"
        & $Body
        Log "OK    $Name"
    } catch {
        $script:Failures += $Name
        Log "FAIL  $Name : $($_.Exception.Message)"
    }
}

function Set-Reg {
    param([string]$Path, [string]$Name, [object]$Value, [string]$Type = 'DWORD')
    try {
        if (-not (Test-Path -LiteralPath $Path)) { New-Item -Path $Path -Force | Out-Null }
        New-ItemProperty -LiteralPath $Path -Name $Name -Value $Value -PropertyType $Type -Force | Out-Null
    } catch {
        Log "WARN  reg $Path\$Name : $($_.Exception.Message)"
    }
}

function Wait-ForProcess {
    param([string]$Name, [int]$TimeoutSeconds = 900)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (Get-Process -Name $Name -ErrorAction SilentlyContinue) { return $true }
        Start-Sleep -Seconds 5
    }
    return $false
}

function Get-ToolsMediaDrive {
    # The VMware Tools ISO is the second CD: it has setup.exe plus manifest.txt.
    foreach ($d in (Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Root -match '^[A-Z]:\\$' })) {
        $root = $d.Root
        if ((Test-Path -LiteralPath ($root + 'manifest.txt')) -and
            (Test-Path -LiteralPath ($root + 'setup.exe')) -and
            (Test-Path -LiteralPath ($root + 'VMwareToolsUpgrader.exe'))) {
            return $root
        }
    }
    return $null
}

function Write-ProbeFiles {
    # Machine readable summary consumed by the host during `base build` verification.
    $probe = [ordered]@{
        schema           = 'bvmenv.provision.v1'
        computer_name    = $env:COMPUTERNAME
        os_version       = (Get-CimInstance Win32_OperatingSystem).Version
        os_caption       = (Get-CimInstance Win32_OperatingSystem).Caption
        architecture     = $env:PROCESSOR_ARCHITECTURE
        tools_installed  = (Test-Path -LiteralPath 'C:\Program Files\VMware\VMware Tools\vmtoolsd.exe')
        admin_user       = 'blook'
        phases_done      = @()
        failures         = @()
        nested_virt_ready = $true
        finished_at      = $null
    }
    $probe.phases_done = @('phase1', 'phase2')
    $probe.failures = $script:Failures
    $probe.finished_at = (Get-Date).ToString('o')
    $probe | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath 'C:\blook\provision.json' -Encoding UTF8
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path 'C:\blook' | Out-Null
Log '=== blook guest provisioning start ==='
Log ('OS: {0} {1} {2}' -f $env:COMPUTERNAME, [System.Environment]::OSVersion.VersionString, $env:PROCESSOR_ARCHITECTURE)
Log ('User: {0} (SYSTEM={1})' -f [System.Security.Principal.WindowsIdentity]::GetCurrent().Name, `
        ([System.Security.Principal.WindowsIdentity]::GetCurrent().IsSystem))

if (Test-Path -LiteralPath 'C:\blook\provision.done') {
    if (-not $Repair) {
        Log 'provision.done present, nothing to do'
        exit 0
    }
    Log 'Repair requested: re-running phase 2'
    Remove-Item -LiteralPath 'C:\blook\provision.done' -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath 'C:\blook\provision.phase2' -Force -ErrorAction SilentlyContinue
}

$phase1Marker = 'C:\blook\provision.phase1'
$phase2Marker = 'C:\blook\provision.phase2'

if (-not (Test-Path -LiteralPath $phase1Marker)) {
    # =======================================================================
    # PHASE 1 - first boot after specialize
    # =======================================================================
    Log 'PHASE 1'

    # Wait until the desktop is up (OOBE done). Do not wait forever: if OOBE
    # is stuck we still want Tools + baseline so the host can inspect it.
    Log 'waiting for explorer.exe (desktop) up to 25 min'
    if (Wait-ForProcess -Name 'explorer' -TimeoutSeconds 1500) { Log 'desktop is up' }
    else { Log 'WARN  desktop did not come up, continuing anyway' }
    Start-Sleep -Seconds 10

    Invoke-Step 'remove bloat tasks/notifications' {
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' 'EnableFirstLogonAnimation' 0
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' 'EnableLUA' 0
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' 'ConsentPromptBehaviorAdmin' 0
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' 'PromptOnSecureDesktop' 0
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' 'FilterAdministratorToken' 0
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization' 'NoLockScreen' 1
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' 'InactivityTimeoutSecs' 0
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon' 'DisableLockWorkstation' 1
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Advanced' 'ShowSecondsInSystemClock' 1
    }

    Invoke-Step 'install vmware tools' {
        $media = Get-ToolsMediaDrive
        if (-not $media) { throw 'VMware Tools media not found (is windows.iso attached?)' }
        Log "tools media: $media"
        $setup = Join-Path $media 'setup.exe'
        $proc = Start-Process -FilePath $setup -ArgumentList '/S', '/v"/qn REBOOT=R ADDLOCAL=ALL"', '/l', 'C:\blook\vmtools-install.log' -Wait -PassThru
        Log ("tools setup exit={0}" -f $proc.ExitCode)
        $deadline = (Get-Date).AddSeconds(600)
        while ((Get-Date) -lt $deadline) {
            if (Test-Path -LiteralPath 'C:\Program Files\VMware\VMware Tools\vmtoolsd.exe') { break }
            Start-Sleep -Seconds 5
        }
        if (-not (Test-Path -LiteralPath 'C:\Program Files\VMware\VMware Tools\vmtoolsd.exe')) {
            throw 'vmtoolsd.exe did not appear after install'
        }
        Set-Service -Name 'VMTools' -StartupType Automatic -ErrorAction SilentlyContinue
        Start-Service -Name 'VMTools' -ErrorAction SilentlyContinue
        $ver = (Get-Item 'C:\Program Files\VMware\VMware Tools\vmtoolsd.exe').VersionInfo
        Log ('tools version: {0}' -f $ver.FileVersion)
    }

    Invoke-Step 'disable vbs hvci device guard' {
        # A nested hypervisor needs VT-x for itself: no VBS/HVCI in the guest.
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard' 'EnableVirtualizationBasedSecurity' 0
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard' 'RequirePlatformSecurityFeatures' 0
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' 'Enabled' 0
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\CredentialGuard' 'Enabled' 0
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\SystemGuard' 'Enabled' 0
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' 'VerifiedAndReputablePolicyState' 0
        & bcdedit.exe /set '{current}' hypervisorlaunchtype off | Out-Null
    }

    Invoke-Step 'install lab test certificate' {
        # Guest test drivers/probes are signed with the lab certificate created
        # by the host and SPC-signed (blook-drv signer scheme); the guest runs with
# DSE enabled, so testsigning must stay OFF.
        # certificate is trusted (Root + TrustedPublisher).
        $cer = 'C:\blook\blook-lab-test.cer'
        if (-not (Test-Path -LiteralPath $cer)) { throw "lab signing certificate missing: $cer" }
        & certutil.exe -addstore -f Root $cer 2>&1 | ForEach-Object { Log ('certutil root: ' + $_) }
        & certutil.exe -addstore -f TrustedPublisher $cer 2>&1 | ForEach-Object { Log ('certutil publisher: ' + $_) }
    }

    Invoke-Step 'test signing + kernel debug' {
        $out = & bcdedit.exe /set '{current}' testsigning off 2>&1
        Log ('testsigning off (DSE on): ' + ($out -join ' '))
        & bcdedit.exe /set '{current}' nointegritychecks on 2>&1 | ForEach-Object { Log ('nointegritychecks: ' + $_) }
        & bcdedit.exe /set '{current}' debug on 2>&1 | ForEach-Object { Log ('debug: ' + $_) }
        & bcdedit.exe /dbgsettings serial debugport:1 baudrate:115200 2>&1 | ForEach-Object { Log ('dbgsettings: ' + $_) }
        & bcdedit.exe /set '{current}' bootstatuspolicy ignoreallfailures 2>&1 | ForEach-Object { Log ('bootstatuspolicy: ' + $_) }
        & bcdedit.exe /set '{current}' recoveryenabled No 2>&1 | ForEach-Object { Log ('recoveryenabled: ' + $_) }
    }

    Invoke-Step 'bugcheck behaviour: stay on screen, no dump' {
        $cc = 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
        Set-Reg $cc 'AutoReboot' 0
        Set-Reg $cc 'DisplayParameters' 1
        Set-Reg $cc 'CrashDumpEnabled' 0
        Set-Reg $cc 'LogEvent' 1
        Set-Reg $cc 'AlwaysKeepMemoryDump' 0
        Set-Reg $cc 'Overwrite' 1
        Set-Reg $cc 'IgnorePagefileSize' 1
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management' 'LargeSystemCache' 0
        # Ctrl+ScrollLock x2 -> bugcheck 0xE2, a crash primitive that needs no
        # test driver (`bvmenv crash`). Remember the last bugcheck as well.
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Services\i8042prt\Parameters' 'CrashOnCtrlScroll' 1
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' 'LastBugCheckTime' 0
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' 'LogEvent' 1
    }

    Invoke-Step 'power: no sleep/hibernate/fast startup' {
        & powercfg.exe /hibernate off | Out-Null
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' 'HiberbootEnabled' 0
        & powercfg.exe /change -standby-timeout-ac 0 | Out-Null
        & powercfg.exe /change -hibernate-timeout-ac 0 | Out-Null
        & powercfg.exe /change -monitor-timeout-ac 0 | Out-Null
        & powercfg.exe /change -disk-timeout-ac 0 | Out-Null
        & powercfg.exe /setactive '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c' | Out-Null
        # Keep the console awake even when the desktop is idle.
        & powercfg.exe /setacvalueindex SCHEME_CURRENT SUB_VIDEO VIDEOIDLE 0 | Out-Null
        & powercfg.exe /setacvalueindex SCHEME_CURRENT SUB_SLEEP STANDBYIDLE 0 | Out-Null
        & powercfg.exe /S SCHEME_CURRENT | Out-Null
    }

    Invoke-Step 'disable windows update' {
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU' 'NoAutoUpdate' 1
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU' 'AUOptions' 1
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate' 'DisableWindowsUpdateAccess' 1
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate' 'DoNotConnectToWindowsUpdateInternetLocations' 1
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update' 'AUOptions' 1
        foreach ($svc in @('wuauserv', 'bits', 'dosvc', 'UsoSvc')) {
            & sc.exe config $svc start= disabled | Out-Null
            & sc.exe stop $svc | Out-Null
        }
        & schtasks.exe /Change /TN '\Microsoft\Windows\UpdateOrchestrator\Schedule Scan' /DISABLE 2>&1 | Out-Null
    }

    Invoke-Step 'disable defender realtime + exclusions' {
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender' 'DisableAntiSpyware' 1
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender' 'DisableAntiVirus' 1
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection' 'DisableRealtimeMonitoring' 1
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection' 'DisableBehaviorMonitoring' 1
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection' 'DisableOnAccessProtection' 1
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection' 'DisableScanOnRealtimeEnable' 1
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows Defender\Features' 'TamperProtection' 0
        try {
            Set-MpPreference -DisableRealtimeMonitoring $true -ErrorAction SilentlyContinue
            Set-MpPreference -DisableIOAVProtection $true -ErrorAction SilentlyContinue
            Set-MpPreference -DisableBehaviorMonitoring $true -ErrorAction SilentlyContinue
            Add-MpPreference -ExclusionPath 'C:\' -ErrorAction SilentlyContinue
            Add-MpPreference -ExclusionProcess 'powershell.exe' -ErrorAction SilentlyContinue
            Add-MpPreference -ExclusionProcess 'cmd.exe' -ErrorAction SilentlyContinue
        } catch { Log "WARN  Set-MpPreference: $($_.Exception.Message)" }
        & sc.exe config WinDefend start= disabled | Out-Null
        & schtasks.exe /Change /TN '\Microsoft\Windows\Windows Defender\Windows Defender Scheduled Scan' /DISABLE 2>&1 | Out-Null
    }

    Invoke-Step 'disable services / telemetry' {
        foreach ($svc in @('SysMain', 'WSearch', 'WerSvc', 'DiagTrack', 'dmwappushservice', 'MapsBroker', 'RetailDemo', 'XblAuthManager')) {
            & sc.exe config $svc start= disabled | Out-Null
            & sc.exe stop $svc | Out-Null
        }
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Windows Error Reporting' 'Disabled' 1
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting' 'Disabled' 1
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DataCollection' 'AllowTelemetry' 0
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' 'EnableSmartScreen' 0 'String'
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters' 'EnablePrefetcher' 0
    }

    Invoke-Step 'disable system restore + startup repair prompts' {
        & vssadmin.exe delete shadows /all /quiet 2>&1 | Out-Null
        & sc.exe config VSS start= disabled | Out-Null
        & sc.exe config SDRSVC start= disabled | Out-Null
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\SystemRestore' 'DisableSR' 1
    }

    Invoke-Step 'nic: static-free dhcp + disable ipv6 isis' {
        Get-NetAdapter -ErrorAction SilentlyContinue | ForEach-Object {
            try {
                Set-NetIPInterface -InterfaceIndex $_.ifIndex -Dhcp Enabled -ErrorAction SilentlyContinue
                Disable-NetAdapterBinding -Name $_.Name -ComponentID 'ms_tcpip6' -ErrorAction SilentlyContinue
            } catch { Log "WARN  nic $($_.Name): $($_.Exception.Message)" }
        }
    }

    Invoke-Step 'enable long paths + dev misc' {
        Set-Reg 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' 'LongPathsEnabled' 1
        Set-Reg 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' 'EnableCursorSuppression' 0
        # Keep symbols/artifacts cheap: no paging of kernel when idle is fine, and
        # make the console font readable for screenshots.
        Set-Reg 'HKCU:\Console' 'FaceName' 'Consolas' 'String'
        Set-Reg 'HKCU:\Console' 'FontSize' 0x00140000
    }

    # Mark phase 1 before rebooting: the boot task runs again on next start.
    Set-Content -LiteralPath $phase1Marker -Value (Get-Date).ToString('o') -Encoding UTF8
    Log 'PHASE 1 complete, rebooting'
    Log ('failures so far: ' + ($script:Failures -join ', '))
    & shutdown.exe /r /f /t 5
    exit 0
}

if (-not (Test-Path -LiteralPath $phase2Marker)) {
    # =======================================================================
    # PHASE 2 - second boot: verify, report, power off cleanly
    # =======================================================================
    Log 'PHASE 2'
    Start-Sleep -Seconds 20

    $verified = @{}
    Invoke-Step 'wait for vmware tools service' {
        $deadline = (Get-Date).AddSeconds(600)
        while ((Get-Date) -lt $deadline) {
            $svc = Get-Service -Name 'VMTools' -ErrorAction SilentlyContinue
            if ($svc -and $svc.Status -eq 'Running') { break }
            Start-Sleep -Seconds 5
        }
        $svc = Get-Service -Name 'VMTools' -ErrorAction SilentlyContinue
        if (-not $svc -or $svc.Status -ne 'Running') { throw 'VMTools service is not running' }
        Log ('VMTools: ' + $svc.Status)
    }

    Invoke-Step 'persistent autologon' {
        # The unattend AutoLogon count only covers the first logons; forks boot
        # many times, so autologon has to live in the registry. Guest operations
        # (vmrun runProgramInGuest) need an interactive session, so a fork that
        # lands on the logon screen is unusable.
        $winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
        Set-Reg $winlogon 'AutoAdminLogon' '1' 'String'
        Set-Reg $winlogon 'DefaultUserName' 'blook' 'String'
        Set-Reg $winlogon 'DefaultPassword' 'blook' 'String'
        Set-Reg $winlogon 'DefaultDomainName' $env:COMPUTERNAME 'String'
        Set-Reg $winlogon 'ForceAutoLogon' '1' 'String'
        Set-Reg $winlogon 'AutoLogonCount' 9999
        # Never let a screensaver / lock screen cover the desktop.
        Set-Reg 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization' 'NoLockScreen' 1
        Set-Reg 'HKCU:\Control Panel\Desktop' 'ScreenSaveActive' '0' 'String'
        $autologon = ((Get-ItemProperty $winlogon -Name AutoAdminLogon -ErrorAction SilentlyContinue).AutoAdminLogon -eq '1')
        if (-not $autologon) { throw 'AutoAdminLogon registry value not set' }
    }

    Invoke-Step 'verify baseline' {
        # Make sure the guest does not launch Microsoft's hypervisor: the lab
        # needs VT-x for a *nested* hypervisor. bcdedit silently drops the value
        # in some configurations, so set it again and record the raw answer.
        & bcdedit.exe /set '{current}' hypervisorlaunchtype off 2>&1 | ForEach-Object { Log ('hypervisorlaunchtype: ' + $_) }
        $bcd = (& bcdedit.exe /enum '{current}' 2>&1) | Out-String
        $bcdLines = (($bcd -split "`r?`n") | Where-Object { $_ -match 'testsigning|nointegritychecks|hypervisorlaunchtype|^debug\b|debugport|bootstatuspolicy' }) -join '; '
        $hypervisorPresent = [bool](Get-CimInstance Win32_ComputerSystem).HypervisorPresent
        $state = [ordered]@{
            testsigning_off      = -not ($bcd -match 'testsigning\s+Yes')
            nointegritychecks    = ($bcd -match '(?m)^nointegritychecks\s+Yes')
            debug                = ($bcd -match '(?m)^debug\s+Yes')
            hypervisorlaunchtype = ((-not $hypervisorPresent) -and ($bcd -match 'hypervisorlaunchtype\s+Off'))
            hypervisor_absent    = (-not $hypervisorPresent)
            hypervisor_present   = $hypervisorPresent
            enablelua            = ((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -Name EnableLUA -ErrorAction SilentlyContinue).EnableLUA -eq 0)
            autoreboot_off       = ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' -Name AutoReboot -ErrorAction SilentlyContinue).AutoReboot -eq 0)
            display_params       = ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' -Name DisplayParameters -ErrorAction SilentlyContinue).DisplayParameters -eq 1)
            hiberboot_off        = ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled -ErrorAction SilentlyContinue).HiberbootEnabled -eq 0)
            vbs_off              = ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard' -Name EnableVirtualizationBasedSecurity -ErrorAction SilentlyContinue).EnableVirtualizationBasedSecurity -eq 0)
            crash_on_ctrl_scroll = ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\i8042prt\Parameters' -Name CrashOnCtrlScroll -ErrorAction SilentlyContinue).CrashOnCtrlScroll -eq 1)
            tools_running        = ((Get-Process -Name vmtoolsd -ErrorAction SilentlyContinue) -ne $null)
            admin_user           = ([bool](Get-LocalUser -Name blook -ErrorAction SilentlyContinue))
            autologon            = ((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon' -Name AutoAdminLogon -ErrorAction SilentlyContinue).AutoAdminLogon -eq '1')
        }
        $script:verified = $state
        $script:bcdLines = $bcdLines
        Log ('baseline: ' + ($state | ConvertTo-Json -Compress))
        Log ('bcdedit: ' + $bcdLines)
        foreach ($k in $state.Keys) { if (-not $state[$k]) { $script:Failures += "verify:$k" } }
    }

    Invoke-Step 'vtx probe (CPUID VMX + IA32_FEATURE_CONTROL)' {
        # Ring 0 probe: user mode cannot read IA32_FEATURE_CONTROL, and a nested
        # hypervisor needs VMX plus VMXON outside SMX.
        $script:vtx = $null
        $probeDriver = 'C:\blook\vtx-probe.sys'
        if (-not (Test-Path -LiteralPath $probeDriver)) {
            throw "vtx probe driver missing: $probeDriver"
        }
        Copy-Item -LiteralPath $probeDriver -Destination 'C:\Windows\System32\drivers\bvmenv-vtx-probe.sys' -Force
        & sc.exe create bvmenv-vtx-probe type= kernel binPath= C:\Windows\System32\drivers\bvmenv-vtx-probe.sys 2>&1 | Out-Null
        & sc.exe start bvmenv-vtx-probe 2>&1 | ForEach-Object { Log ('vtx probe: ' + $_) }
        Start-Sleep -Seconds 2
        & sc.exe stop bvmenv-vtx-probe 2>&1 | Out-Null
        & sc.exe delete bvmenv-vtx-probe 2>&1 | Out-Null
        $values = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\blook\vtx-probe' -ErrorAction SilentlyContinue
        if ($null -eq $values) { throw 'vtx probe did not write HKLM\SOFTWARE\blook\vtx-probe' }
        $script:vtx = [ordered]@{
            cpuid_vmx              = [int]$values.CpuidVmx
            hypervisor_present     = [int]$values.HypervisorPresent
            feature_control_locked = [int]$values.FeatureControlLocked
            vmxon_outside_smx      = [int]$values.VmxOnOutsideSmx
            feature_control_low    = ('0x{0:x8}' -f [int]$values.FeatureControlLow)
            vmx_basic_low          = ('0x{0:x8}' -f [int]$values.VmxBasicLow)
            ok                     = ([int]$values.ProbeResult -eq 1)
        }
        Log ('vtx probe: ' + ($script:vtx | ConvertTo-Json -Compress))
        if (-not $script:vtx.ok) { $script:Failures += 'verify:vtx_probe' }
    }

    Invoke-Step 'write provision probe' {
        $probe = [ordered]@{
            schema            = 'bvmenv.provision.v1'
            computer_name     = $env:COMPUTERNAME
            os_version        = (Get-CimInstance Win32_OperatingSystem).Version
            os_caption        = (Get-CimInstance Win32_OperatingSystem).Caption
            architecture      = $env:PROCESSOR_ARCHITECTURE
            tools_installed   = (Test-Path -LiteralPath 'C:\Program Files\VMware\VMware Tools\vmtoolsd.exe')
            tools_version     = (Get-Item 'C:\Program Files\VMware\VMware Tools\vmtoolsd.exe' -ErrorAction SilentlyContinue).VersionInfo.FileVersion
            admin_user        = 'blook'
            autologon         = ((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon' -Name AutoAdminLogon -ErrorAction SilentlyContinue).AutoAdminLogon -eq '1')
            baseline          = $script:verified
            bcdedit_lines     = $script:bcdLines
            nested_virt       = $script:vtx
            phases_done       = @('phase1', 'phase2')
            failures          = $script:Failures
            finished_at       = (Get-Date).ToString('o')
        }
        $probe | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath 'C:\blook\provision.json' -Encoding UTF8
        Log ('provision.json written; failures: ' + (($script:Failures -join ',')))
    }

    Set-Content -LiteralPath $phase2Marker -Value (Get-Date).ToString('o') -Encoding UTF8
    Invoke-Step 'cleanup provisioning task' {
        & schtasks.exe /delete /tn blook-provision /f 2>&1 | Out-Null
        & schtasks.exe /delete /tn '\Microsoft\Windows\UpdateOrchestrator\Schedule Scan' /f 2>&1 | Out-Null
    }
    Set-Content -LiteralPath 'C:\blook\provision.done' -Value (Get-Date).ToString('o') -Encoding UTF8
    # Trust any CA certificates staged by the host (e.g. blook-drv's signer tsa
    # root + VeriSign chain) so SPC-signed drivers can at least be verified with
    # signtool /kp inside the VM.
    $certDir = 'C:\blook\certs'
    if (Test-Path -LiteralPath $certDir) {
        Get-ChildItem -LiteralPath $certDir -Filter *.cer -ErrorAction SilentlyContinue | ForEach-Object {
            & certutil.exe -addstore -f Root $_.FullName | Out-Null
            Log ('trusted cert: ' + $_.Name)
        }
    }
    # --- SPC trust imports (DSE on) ---------------------------------------
    # Verified on Win11 26100: SPC **SHA-1** signatures + these imports load with
    # DSE enforced; SHA-256 signatures are rejected. Staged by the host in
    # C:\blook\certs (blook-drv's signer: tsa root + VeriSign chain) and
    # C:\blook\*.sys (SPC-signed drivers whose signer certificate is exported
    # into TrustedPublisher).
    $certDir = 'C:\blook\certs'
    $trustPlan = @(
        @{ Store = 'Root'; File = 'tsa-root.cer' },
        @{ Store = 'CA'; File = 'verisign-cscs-2010.cer' },
        @{ Store = 'CA'; File = 'verisign-g5-mcvr.cer' }
    )
    foreach ($item in $trustPlan) {
        $path = Join-Path $certDir $item.File
        if (Test-Path -LiteralPath $path) {
            & certutil.exe -addstore -f $item.Store $path | Out-Null
            Log ('trust: ' + $item.Store + ' <- ' + $item.File)
        }
    }
    # The signer (XINDA) leaf certificate is staged by the host as a file: inside
    # phase 2 the chain is not trusted yet, so Get-AuthenticodeSignature returns
    # no signer certificate and the export silently produced nothing (that is why
    # DSE kept rejecting the drivers with 577).
    foreach ($leafName in @('xinda.cer')) {
        $leaf = Join-Path $certDir $leafName
        if (Test-Path -LiteralPath $leaf) {
            & certutil.exe -addstore -f TrustedPublisher $leaf | Out-Null
            Log ('trust: TrustedPublisher <- ' + $leafName)
        }
    }

    Log 'PHASE 2 complete, shutting down'
    Start-Sleep -Seconds 2
    & shutdown.exe /s /f /t 5
    exit 0
}

Log 'unexpected state, exiting'
exit 1
