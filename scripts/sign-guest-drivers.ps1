<#
.SYNOPSIS
Signs the guest test drivers so SCM can load them in the lab image.

.DESCRIPTION
Two modes:

* `lab` (default) - SHA-256 Authenticode with the lab test certificate
  (`CN=blook-lab-test`). This is what the lab image needs: it runs with
  `testsigning on`, and Windows 11 26100 rejects SHA-1 SPC signatures outright
  (VERIFY: `sc start` -> 577 INVALID_IMAGE_HASH, both with testsigning on and off,
  even with the full chain trusted). See docs/images.md.
* `spc` - blook-drv's SPC scheme (spcsign, XINDA cert, 2013 countersignature).
  Kept for CI/other guests; the signer is not vendored here, point
  $env:BVMENV_SIGNER_DIR at a copy of blook-drv's `signer/`.

.EXAMPLE
pwsh -File scripts/sign-guest-drivers.ps1 -Mode lab
pwsh -File scripts/sign-guest-drivers.ps1 -Mode spc
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\windows\x64\releasedbg'),
    [ValidateSet('lab', 'spc')][string]$Mode = 'lab',
    [string]$SignerDir = $(if ($env:BVMENV_SIGNER_DIR) { $env:BVMENV_SIGNER_DIR } else { 'C:\blook-vms\_signer' })
)

$ErrorActionPreference = 'Stop'
$drivers = Get-ChildItem -Path $BuildDir -Filter 'guest-*.sys' -File -ErrorAction SilentlyContinue
if (-not $drivers) { throw "no guest-*.sys in $BuildDir (build with --guest_drivers=y)" }

if ($Mode -eq 'spc') {
    $spc = Join-Path $SignerDir 'spcsign\spcsign.exe'
    if (-not (Test-Path -LiteralPath $spc)) {
        throw "spcsign.exe not found at $spc. Set BVMENV_SIGNER_DIR to blook-drv's signer/ directory."
    }
    $chains = @(
        (Join-Path $SignerDir 'certs\verisign-cscs-2010.cer'),
        (Join-Path $SignerDir 'certs\verisign-g5-mcvr.cer')
    )
    foreach ($driver in $drivers) {
        $args = @('sign', '--sha1', '--cert', 'XINDA', '--tsa-time', '2013-01-01T00:00:00Z')
        foreach ($chain in $chains) { $args += @('--chain', $chain) }
        $args += $driver.FullName
        $out = & $spc @args 2>&1
        foreach ($line in $out) { Write-Host "  $($driver.Name): $line" }
        if ($LASTEXITCODE -ne 0) { throw "spcsign failed for $($driver.Name) ($LASTEXITCODE)" }
    }
    Write-Host "SPC(SHA-1)-signed $($drivers.Count) driver(s) in $BuildDir"
    exit 0
}

# lab mode: SHA-256 with the lab test certificate
$signtool = Get-ChildItem 'Y:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
if (-not $signtool) {
    $signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
}
if (-not $signtool) { throw 'signtool.exe not found (install the Windows SDK)' }

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -match 'CN=blook-lab-test' } | Select-Object -First 1
if (-not $cert) { throw 'lab signing certificate CN=blook-lab-test not found in Cert:\CurrentUser\My' }

foreach ($driver in $drivers) {
    $out = & $signtool.FullName sign /sha1 $cert.Thumbprint /fd SHA256 $driver.FullName 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host ($out -join "`n")
        throw "signtool failed for $($driver.Name)"
    }
    Write-Host "  lab(SHA-256) signed: $($driver.Name)"
}
Write-Host "lab-signed $($drivers.Count) driver(s) in $BuildDir"
