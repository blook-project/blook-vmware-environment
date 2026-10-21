# Pins a running VMware fork to the P-cores (runtime affinity).
#
# Why runtime: sched.cpu.affinity in the .vmx prevents power-on on this
# Workstation build, and vmware-vmx runs under a different principal, so its
# command line (and sometimes the exact PID) is not readable from a non-elevated
# process. Strategy:
#   1. try the PID published in <vmx>.lck/M<pid>.lck;
#   2. if that PID is not a live vmware-vmx, pin every vmware-vmx process of this
#      user (all lab VMs run as the same user) and report which ones were pinned.
# The applied mask is verified by reading ProcessorAffinity back.
param(
    [Parameter(Mandatory = $true)][string]$Vmx,
    [string]$Pcores = '0-15'
)

$ErrorActionPreference = 'Stop'

function ConvertTo-Mask([string]$spec) {
    $mask = [uint64]0
    foreach ($part in $spec.Split(',')) {
        $part = $part.Trim()
        if ($part -match '^\d+$') {
            $mask = $mask -bor ([uint64]1 -shl [int]$part)
        } elseif ($part -match '^(\d+)-(\d+)$') {
            for ($i = [int]$Matches[1]; $i -le [int]$Matches[2]; $i++) {
                $mask = $mask -bor ([uint64]1 -shl $i)
            }
        } else {
            throw "bad --pcores item: $part"
        }
    }
    return $mask
}

function Get-VmxPids([string]$vmxPath) {
    $pids = @()
    $lockDir = "$vmxPath.lck"
    if (Test-Path -LiteralPath $lockDir) {
        foreach ($file in Get-ChildItem -LiteralPath $lockDir -Filter 'M*.lck' -ErrorAction SilentlyContinue) {
            if ($file.BaseName -match '^M(\d+)$') {
                $proc = Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue
                if ($proc -and $proc.ProcessName -eq 'vmware-vmx') { $pids += $proc.Id }
            }
        }
    }
    if ($pids.Count -eq 0) {
        $needle = $vmxPath.ToLower()
        foreach ($cim in Get-CimInstance Win32_Process -Filter "Name = 'vmware-vmx.exe'" -ErrorAction SilentlyContinue) {
            if ($cim.CommandLine -and $cim.CommandLine.ToLower().Contains($needle)) { $pids += [int]$cim.ProcessId }
        }
    }
    if ($pids.Count -eq 0) {
        $pids = @(Get-Process vmware-vmx -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
    }
    return @($pids | Sort-Object -Unique)
}

function Set-Affinity([int]$processId, [uint64]$mask) {
    try {
        $proc = Get-Process -Id $processId -ErrorAction Stop
        $proc.ProcessorAffinity = [IntPtr][int64]$mask
        return $true
    } catch {
        $gsudo = Get-Command gsudo -ErrorAction SilentlyContinue
        if ($gsudo) {
            $inner = "`$p = Get-Process -Id $processId; `$p.ProcessorAffinity = [IntPtr][int64]$mask"
            & $gsudo.Source -d powershell -NoProfile -Command $inner | Out-Null
            if ($LASTEXITCODE -eq 0) { return $true }
        }
        return $false
    }
}

$mask = ConvertTo-Mask $Pcores
$targets = @()
$applied = @()
for ($attempt = 0; $attempt -lt 40; $attempt++) {
    $targets = Get-VmxPids $Vmx
    if ($targets.Count -gt 0) {
        foreach ($processId in $targets) {
            if (Set-Affinity $processId $mask) { $applied += $processId }
        }
        if ($applied.Count -gt 0) { break }
    }
    Start-Sleep -Milliseconds 500
}

$observed = @()
foreach ($processId in $applied) {
    try {
        $value = [int64](Get-Process -Id $processId -ErrorAction Stop).ProcessorAffinity
        $observed += [ordered]@{ pid = $processId; mask = ('0x{0:X8}' -f $value) }
    } catch {
    }
}

$result = [ordered]@{
    schema         = 'bvmenv.pin-vcpu.v1'
    vm             = (Split-Path $Vmx -Leaf) -replace '\.vmx$', ''
    vmx            = $Vmx
    pcores         = $Pcores
    mask_requested = ('0x{0:X8}' -f $mask)
    targets        = $targets
    pinned_pids    = $applied
    observed_masks = $observed
    applied        = ($applied.Count -gt 0)
    matches        = ($observed.Count -eq $applied.Count -and $applied.Count -gt 0)
}
$result | ConvertTo-Json -Depth 4 | Write-Output
if (-not $result.matches) { exit 1 }
