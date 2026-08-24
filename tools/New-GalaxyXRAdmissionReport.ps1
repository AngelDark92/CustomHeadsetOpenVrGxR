[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet(5002318, 5002322)]
    [int]$VersionCode,
    [Parameter(Mandatory)]
    [string]$ApkPath,
    [Parameter(Mandatory)]
    [string]$BridgeLibraryPath,
    [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'
$apk = (Resolve-Path -LiteralPath $ApkPath).Path
$bridge = (Resolve-Path -LiteralPath $BridgeLibraryPath).Path
if ([IO.Path]::GetExtension($apk) -cne '.apk') { throw 'ApkPath must point to the final signed APK.' }
if ([IO.Path]::GetFileName($bridge) -cne 'libgxr_xr_bridge.so') {
    throw 'BridgeLibraryPath must point to libgxr_xr_bridge.so used in that APK.'
}
if (-not $OutputPath) {
    $OutputPath = Join-Path (Split-Path -Parent $apk) "galaxyxr-admission-$VersionCode.json"
}
$outputParent = Split-Path -Parent ([IO.Path]::GetFullPath($OutputPath))
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    throw "Output directory does not exist: $outputParent"
}

$record = [ordered]@{
    schemaVersion = 1
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    finalApk = [ordered]@{
        fileName = [IO.Path]::GetFileName($apk)
        sha256 = (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    allowedClient = [ordered]@{
        versionCode = $VersionCode
        apkSha256 = (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLowerInvariant()
        bridgeSha256 = (Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$json = $record | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputPath), $json, [Text.UTF8Encoding]::new($false))
Write-Host "Admission report written: $([IO.Path]::GetFullPath($OutputPath))"
Write-Host 'Copy only allowedClient into galaxyXR.telemetry.allowedClients. Pairing secrets are intentionally excluded.'
