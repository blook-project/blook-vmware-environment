<#
.SYNOPSIS
SPC-signs the guest test drivers with the blook-drv signer scheme.

.DESCRIPTION
The guests run with Driver Signature Enforcement ON (testsigning off), so the
drivers must carry a real SPC chain: spcsign (blook-drv `signer/`) + the XINDA
class-3 certificate + a 2013 countersignature, byte-for-byte the shape that
passes kernel policy.

The signer is NOT vendored in this repo (it belongs to blook-drv). Point
$env:BVMENV_SIGNER_DIR at a copy of that `signer/` directory (default
`C:\blook-vms\_signer`). The signer walks up from its own path to find
`spc-templates/` and `tsa/` next to it.

.EXAMPLE
pwsh -File scripts/sign-guest-drivers.ps1 -BuildDir build/windows/x64/releasedbg
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..uild\windowsdeleasedbg'),
    [string]$SignerDir = $(if ($env:BVMENV_SIGNER_DIR) { $env:BVMENV_SIGNER_DIR } else { 'C:\blook-vms\_signer' })
)

$ErrorActionPreference = 'Stop'
$spc = Join-Path $SignerDir 'spcsign\spcsign.exe'
if (-not (Test-Path -LiteralPath $spc)) {
    throw "spcsign.exe not found at $spc. Set BVMENV_SIGNER_DIR to blook-drv's signer/ directory."
}
$chains = @(
    (Join-Path $SignerDir 'certserisign-cscs-2010.cer'),
    (Join-Path $SignerDir 'certserisign-g5-mcvr.cer')
)
foreach ($chain in $chains) {
    if (-not (Test-Path -LiteralPath $chain)) { throw "chain cert missing: $chain" }
}

$drivers = Get-ChildItem -Path $BuildDir -Filter 'guest-*.sys' -File -ErrorAction SilentlyContinue
if (-not $drivers) { throw "no guest-*.sys in $BuildDir (build with --guest_drivers=y)" }

foreach ($driver in $drivers) {
    $args = @('sign', '--sha1', '--cert', 'XINDA', '--tsa-time', '2013-01-01T00:00:00Z')
    foreach ($chain in $chains) { $args += @('--chain', $chain) }
    $args += $driver.FullName
    $out = & $spc @args 2>&1
    foreach ($line in $out) { Write-Host "  $($driver.Name): $line" }
    if ($LASTEXITCODE -ne 0) { throw "spcsign failed for $($driver.Name) ($LASTEXITCODE)" }
}
Write-Host "SPC-signed $($drivers.Count) driver(s) in $BuildDir"
