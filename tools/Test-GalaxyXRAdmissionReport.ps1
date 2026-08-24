[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = [IO.Path]::GetFullPath((Join-Path $tempRoot ('galaxyxr-admission-' + [Guid]::NewGuid().ToString('N'))))
if (-not $testRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or $testRoot -eq $tempRoot) {
    throw 'Refusing unsafe test directory.'
}
[IO.Directory]::CreateDirectory($testRoot) | Out-Null

try {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    function New-TestApk {
        param([string]$Path, [int]$BridgeEntryCount)
        $zip = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Create)
        try {
            for ($index = 0; $index -lt $BridgeEntryCount; $index++) {
                $entry = $zip.CreateEntry('lib/arm64-v8a/libgxr_xr_bridge.so')
                $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
                try { $writer.Write('embedded-bridge-test-payload') } finally { $writer.Dispose() }
            }
        } finally { $zip.Dispose() }
    }

    $apk = Join-Path $testRoot 'final.apk'
    New-TestApk -Path $apk -BridgeEntryCount 1

    $settingsPath = Join-Path $testRoot 'settings.json'
    $preExistingRollback = $settingsPath + '.rollback'
    [IO.File]::WriteAllText($settingsPath, '{"unrelated":{"keep":true},"galaxyXR":{"telemetry":{"allowedClients":[{"versionCode":5002318,"apkSha256":"old-apk","bridgeSha256":"old-bridge"}]}}}', [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($preExistingRollback, 'user-recovery-data', [Text.UTF8Encoding]::new($false))
    $reportPath = Join-Path $testRoot 'galaxyxr-admission-5002322.json'
    & (Join-Path $PSScriptRoot 'Set-GalaxyXRNativeTelemetryHost.ps1') `
        -ApkPath $apk `
        -VersionCode 5002322 `
        -ListenAddress '192.168.1.27' `
        -PairingTokenHex ('Ab' * 32) `
        -SettingsPath $settingsPath

    $report = Get-Content -Raw -LiteralPath $reportPath | ConvertFrom-Json
    $settings = Get-Content -Raw -LiteralPath $settingsPath | ConvertFrom-Json
    if ($report.allowedClient.apkSha256 -ne (Get-FileHash $apk -Algorithm SHA256).Hash.ToLowerInvariant()) { throw 'APK hash mismatch.' }
    $bridgeBytes = [Text.Encoding]::UTF8.GetBytes('embedded-bridge-test-payload')
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $expectedBridgeHash = ([BitConverter]::ToString($sha.ComputeHash($bridgeBytes))).Replace('-', '').ToLowerInvariant() } finally { $sha.Dispose() }
    if ($report.allowedClient.bridgeSha256 -ne $expectedBridgeHash) { throw 'Embedded bridge hash mismatch.' }
    if ($settings.unrelated.keep -ne $true) { throw 'Unrelated settings were not preserved.' }
    if ($settings.galaxyXR.telemetry.controlPort -ne 29981 -or $settings.galaxyXR.telemetry.trackingPort -ne 29982) { throw 'Standard ports were not applied.' }
    if ($settings.galaxyXR.telemetry.pairingTokenHex -ne ('ab' * 32)) { throw 'Pairing token mismatch.' }
    if ((Get-Content -Raw -LiteralPath $preExistingRollback) -ne 'user-recovery-data') { throw 'Pre-existing recovery data was modified.' }
    if (@($settings.galaxyXR.telemetry.allowedClients).Count -ne 2) { throw 'Admission merge did not preserve the other build.' }
    $current = @($settings.galaxyXR.telemetry.allowedClients | Where-Object versionCode -eq 5002322)
    if ($current.Count -ne 1 -or $current[0].bridgeSha256 -ne $report.allowedClient.bridgeSha256) { throw 'Current admission was not enrolled.' }

    foreach ($entryCount in @(0, 2)) {
        $invalidApk = Join-Path $testRoot "bridge-count-$entryCount.apk"
        New-TestApk -Path $invalidApk -BridgeEntryCount $entryCount
        try {
            & (Join-Path $PSScriptRoot 'New-GalaxyXRAdmissionReport.ps1') -ApkPath $invalidApk -VersionCode 5002322 -ListenAddress '192.168.1.27' -PairingTokenHex ('ab' * 32) -OutputPath (Join-Path $testRoot "invalid-$entryCount.json")
            throw "Expected bridge-count $entryCount rejection."
        } catch {
            if ($_.Exception.Message -notmatch 'exactly one') { throw }
        }
    }

    try {
        & (Join-Path $PSScriptRoot 'New-GalaxyXRAdmissionReport.ps1') -ApkPath $apk -VersionCode 5002322 -ListenAddress '192.168.1.27' -PairingTokenHex ('0' * 64) -OutputPath (Join-Path $testRoot 'weak-token.json')
        throw 'Expected uniform token rejection.'
    } catch {
        if ($_.Exception.Message -notmatch 'more than one distinct') { throw }
    }

    try {
        & (Join-Path $PSScriptRoot 'New-GalaxyXRAdmissionReport.ps1') -ApkPath $apk -VersionCode 5002322 -ListenAddress '224.0.0.1' -PairingTokenHex ('ab' * 32) -OutputPath (Join-Path $testRoot 'multicast-host.json')
        throw 'Expected multicast host rejection.'
    } catch {
        if ($_.Exception.Message -notmatch 'usable unicast') { throw }
    }

    $whatIfReport = Join-Path $testRoot 'whatif.json'
    & (Join-Path $PSScriptRoot 'New-GalaxyXRAdmissionReport.ps1') -ApkPath $apk -VersionCode 5002322 -ListenAddress '192.168.1.27' -PairingTokenHex ('ab' * 32) -OutputPath $whatIfReport -WhatIf
    if (Test-Path -LiteralPath $whatIfReport) { throw 'WhatIf wrote the report.' }

    Write-Host 'Galaxy XR admission automation test passed.'
} finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
