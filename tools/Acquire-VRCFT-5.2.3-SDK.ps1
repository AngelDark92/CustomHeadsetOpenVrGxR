[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$releaseUri = 'https://github.com/benaclejames/VRCFaceTracking/releases/download/5.2.3.0/VRCFaceTracking_5.2.3.0_x64.zip'
$archiveSha256 = 'CD711A1E1AA20A8EBFB7A1CD13B0C671BF237AD678EE175DF230F753B315541F'
$coreSha256 = '3D04AA1392BD495B05FC8B844596D1B24A8FAAEAB34493851D9437931A2E4601'
$sdkSha256 = '7DB9C7F05C5D5AC8F6A2C584F9508D514643DA8D63CE6F1C00EF28D7B84D8150'
$repoRoot = Split-Path -Parent $PSScriptRoot
$destinationRoot = Join-Path $repoRoot 'ThirdParty\VRCFaceTracking\5.2.3.0'
$temporaryRoot = Join-Path $repoRoot '.tmp\vrcft-sdk-acquire'
$archive = Join-Path $temporaryRoot 'VRCFaceTracking_5.2.3.0_x64.zip'
$extractRoot = Join-Path $temporaryRoot 'extract'

New-Item -ItemType Directory -Force $temporaryRoot, $extractRoot | Out-Null
Invoke-WebRequest -Uri $releaseUri -OutFile $archive
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $archiveSha256) {
    throw 'VRCFaceTracking 5.2.3.0 release archive hash mismatch.'
}

# The upstream release intentionally password-protects its archive; the
# password is published in that release's notes.
& tar --passphrase kobold -xf $archive -C $extractRoot VRCFaceTracking.Core.dll VRCFaceTracking.SDK.dll
if ($LASTEXITCODE -ne 0) { throw 'Failed to extract VRCFaceTracking.Core.dll.' }
$core = Join-Path $extractRoot 'VRCFaceTracking.Core.dll'
$sdk = Join-Path $extractRoot 'VRCFaceTracking.SDK.dll'
if ((Get-FileHash -LiteralPath $core -Algorithm SHA256).Hash -ne $coreSha256) {
    throw 'VRCFaceTracking.Core.dll hash mismatch.'
}
if ((Get-FileHash -LiteralPath $sdk -Algorithm SHA256).Hash -ne $sdkSha256) {
    throw 'VRCFaceTracking.SDK.dll hash mismatch.'
}

New-Item -ItemType Directory -Force $destinationRoot | Out-Null
Copy-Item -LiteralPath $core -Destination (Join-Path $destinationRoot 'VRCFaceTracking.Core.dll') -Force
Copy-Item -LiteralPath $sdk -Destination (Join-Path $destinationRoot 'VRCFaceTracking.SDK.dll') -Force
Write-Host "Verified VRCFaceTracking 5.2.3.0 SDK at $destinationRoot"
