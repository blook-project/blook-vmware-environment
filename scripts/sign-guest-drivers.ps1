# Sign the in-guest test drivers/drivers used by e2e bundles with the lab test
# certificate, so they load in a `testsigning on` guest.
#
#   pwsh scripts/sign-guest-drivers.ps1 [-BuildDir build\windows\x64\releasedbg]
#
# The same certificate is created by `bvmenv base build` and staged into the
# guest trust stores by provisioning, so a signed driver loaded from an e2e
# bundle works exactly like one inside the image.
param(
    [string]$BuildDir = 'build\windows\x64\releasedbg'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $repo $BuildDir
if (-not (Test-Path $dir)) { throw "build dir not found: $dir" }

# Certificate: reuse the lab certificate created by `bvmenv base build`, or
# create it here (same subject, CurrentUser store).
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=blook-lab-test' } |
    Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=blook-lab-test' `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(5)
    Write-Host "created lab signing certificate $($cert.Thumbprint)"
}

$signtool = Get-ChildItem 'Y:\Program Files (x86)\Windows Kits\10\bin', 'C:\Program Files (x86)\Windows Kits\10\bin' `
    -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -Last 1
if (-not $signtool) { throw 'signtool.exe not found under Windows Kits\10\bin' }

$sys = Get-ChildItem $dir -Filter '*.sys' -ErrorAction SilentlyContinue
if (-not $sys) { throw "no .sys files in $dir (build with --guest_drivers=y)" }
foreach ($file in $sys) {
    & $signtool sign /sha1 $cert.Thumbprint /s My /fd SHA256 $file.FullName | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $($file.Name)" }
    Write-Host "signed $($file.Name)"
}
