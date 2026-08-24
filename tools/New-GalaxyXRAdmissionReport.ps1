[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string]$ApkPath,
    [ValidateScript({ $_ -eq 0 -or $_ -in @(5002318, 5002322) })]
    [int]$VersionCode = 0,
    [string]$ListenAddress = '',
    [string]$PairingTokenHex = '',
    [string]$ApkAnalyzerPath = '',
    [string]$OutputPath = '',
    [switch]$ApplyToSettings,
    [string]$SettingsPath = ''
)

$ErrorActionPreference = 'Stop'
$ControlPort = 29981
$TrackingPort = 29982
$BridgeEntryName = 'lib/arm64-v8a/libgxr_xr_bridge.so'
$AndroidNamespace = 'http://schemas.android.com/apk/res/android'

function Resolve-ApkAnalyzer {
    param([string]$ExplicitPath)

    $candidates = @()
    if ($ExplicitPath) { $candidates += $ExplicitPath }
    if ($env:ANDROID_HOME) {
        $candidates += (Join-Path $env:ANDROID_HOME 'cmdline-tools\latest\bin\apkanalyzer.bat')
    }
    $candidates += (Join-Path $PSScriptRoot '..\..\.android-sdk\cmdline-tools\latest\bin\apkanalyzer.bat')
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'Android apkanalyzer was not found. Pass -ApkAnalyzerPath, or provide -VersionCode, -ListenAddress, and -PairingTokenHex explicitly.'
}

function Get-ManifestMetadata {
    param([string]$Analyzer, [string]$Apk)

    $manifestText = (& $Analyzer manifest print $Apk | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $manifestText) {
        throw 'apkanalyzer could not decode AndroidManifest.xml from the final APK.'
    }
    try { [xml]$manifest = $manifestText } catch {
        throw "apkanalyzer returned invalid manifest XML: $($_.Exception.Message)"
    }
    if ($manifest.manifest.package -ne 'com.valvesoftware.steamlinkvr') {
        throw "Unexpected APK package: $($manifest.manifest.package)"
    }

    $metadata = @{}
    foreach ($node in $manifest.SelectNodes('/manifest/application/meta-data')) {
        $name = $node.GetAttribute('name', $AndroidNamespace)
        if ($name) { $metadata[$name] = $node.GetAttribute('value', $AndroidNamespace) }
    }
    return $metadata
}

function Get-EmbeddedBridgeSha256 {
    param([string]$Apk)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Apk)
    try {
        $matches = @($archive.Entries | Where-Object { $_.FullName -ceq $BridgeEntryName })
        if ($matches.Count -ne 1) {
            throw "Final APK must contain exactly one $BridgeEntryName entry; found $($matches.Count)."
        }
        $stream = $matches[0].Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
        } finally {
            $sha.Dispose()
            $stream.Dispose()
        }
    } finally {
        $archive.Dispose()
    }
}

function Set-ObjectProperty {
    param([object]$Object, [string]$Name, [object]$Value)
    $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force
}

function Write-Utf8FileAtomically {
    param([string]$Path, [string]$Content)

    $fullPath = [IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $fullPath
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        [IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    $stage = Join-Path $parent ('.' + [IO.Path]::GetFileName($fullPath) + '.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $backup = $fullPath + '.' + [Guid]::NewGuid().ToString('N') + '.rollback'
    try {
        [IO.File]::WriteAllText($stage, $Content, [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
            [IO.File]::Replace($stage, $fullPath, $backup, $true)
            try {
                Remove-Item -LiteralPath $backup -Force
            } catch {
                Write-Warning "Updated $fullPath, but could not remove the owned recovery file: $backup"
            }
        } else {
            [IO.File]::Move($stage, $fullPath)
        }
    } catch {
        $originalError = $_
        if (Test-Path -LiteralPath $backup -PathType Leaf) {
            try {
                if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
                    [IO.File]::Replace($backup, $fullPath, $null, $true)
                } else {
                    [IO.File]::Move($backup, $fullPath)
                }
            } catch {
                throw "Write failed and rollback also failed. Recovery file retained at $backup. Original error: $($originalError.Exception.Message). Rollback error: $($_.Exception.Message)"
            }
        }
        throw $originalError
    } finally {
        if (Test-Path -LiteralPath $stage -PathType Leaf) { Remove-Item -LiteralPath $stage -Force }
    }
}

$apk = (Resolve-Path -LiteralPath $ApkPath).Path
if ([IO.Path]::GetExtension($apk) -cne '.apk') { throw 'ApkPath must point to the final signed APK.' }

if (-not $VersionCode -or -not $ListenAddress -or -not $PairingTokenHex) {
    $analyzer = Resolve-ApkAnalyzer -ExplicitPath $ApkAnalyzerPath
    $manifestMetadata = Get-ManifestMetadata -Analyzer $analyzer -Apk $apk
    if (-not $VersionCode) {
        $parsedVersionCode = 0
        if (-not [int]::TryParse($manifestMetadata['gxr.build.versionCode'], [ref]$parsedVersionCode)) {
            throw 'Final APK lacks a valid gxr.build.versionCode metadata value.'
        }
        $VersionCode = $parsedVersionCode
    }
    if (-not $ListenAddress) { $ListenAddress = $manifestMetadata['gxr.telemetry.host'] }
    if (-not $PairingTokenHex) { $PairingTokenHex = $manifestMetadata['gxr.telemetry.pairingTokenHex'] }
    if ($manifestMetadata['gxr.telemetry.enabled'] -ne 'true') {
        throw 'Final APK does not have Galaxy XR native telemetry enabled.'
    }
    if ($manifestMetadata['gxr.telemetry.controlPort'] -ne "$ControlPort" -or
        $manifestMetadata['gxr.telemetry.trackingPort'] -ne "$TrackingPort") {
        throw "Final APK telemetry ports must be the standard $ControlPort/$TrackingPort pair."
    }
}

if ($VersionCode -notin @(5002318, 5002322)) { throw "Unsupported Steam Link version code: $VersionCode" }
$parsedIp = $null
if (-not [Net.IPAddress]::TryParse($ListenAddress, [ref]$parsedIp) -or
    $parsedIp.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
    $parsedIp.Equals([Net.IPAddress]::Any) -or $parsedIp.Equals([Net.IPAddress]::Loopback) -or
    $parsedIp.GetAddressBytes()[0] -eq 0 -or $parsedIp.GetAddressBytes()[0] -ge 224) {
    throw 'ListenAddress must be a usable unicast, non-loopback IPv4 address.'
}
$normalizedPairingToken = $PairingTokenHex.ToLowerInvariant()
if ($normalizedPairingToken -notmatch '^[0-9a-f]{64}$' -or
    @($normalizedPairingToken.ToCharArray() | Select-Object -Unique).Count -le 1) {
    throw 'PairingTokenHex must contain exactly 64 hexadecimal characters and more than one distinct character.'
}
$PairingTokenHex = $normalizedPairingToken

$apkHash = (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLowerInvariant()
$bridgeHash = Get-EmbeddedBridgeSha256 -Apk $apk
if (-not $OutputPath) {
    $OutputPath = Join-Path (Split-Path -Parent $apk) "galaxyxr-admission-$VersionCode.json"
}
$record = [ordered]@{
    schemaVersion = 2
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    finalApk = [ordered]@{
        fileName = [IO.Path]::GetFileName($apk)
        sha256 = $apkHash
    }
    allowedClient = [ordered]@{
        versionCode = $VersionCode
        apkSha256 = $apkHash
        bridgeSha256 = $bridgeHash
    }
}
$reportPath = [IO.Path]::GetFullPath($OutputPath)
if ($PSCmdlet.ShouldProcess($reportPath, 'Write non-secret APK admission report')) {
    Write-Utf8FileAtomically -Path $reportPath -Content ($record | ConvertTo-Json -Depth 5)
    Write-Host "Admission report written: $reportPath"
}

if ($ApplyToSettings) {
    if (-not $SettingsPath) {
        if (-not $env:APPDATA) { throw 'APPDATA is unavailable; pass -SettingsPath explicitly.' }
        $SettingsPath = Join-Path $env:APPDATA 'GalaxyXR\CustomHeadset\settings.json'
    }
    $settingsFullPath = [IO.Path]::GetFullPath($SettingsPath)
    if (Test-Path -LiteralPath $settingsFullPath -PathType Leaf) {
        $settings = Get-Content -Raw -LiteralPath $settingsFullPath | ConvertFrom-Json
    } else {
        $settings = [pscustomobject]@{}
    }
    if (-not $settings.galaxyXR) { Set-ObjectProperty -Object $settings -Name 'galaxyXR' -Value ([pscustomobject]@{}) }
    Set-ObjectProperty -Object $settings.galaxyXR -Name 'enable' -Value $true
    if (-not $settings.galaxyXR.telemetry) {
        Set-ObjectProperty -Object $settings.galaxyXR -Name 'telemetry' -Value ([pscustomobject]@{})
    }
    $telemetry = $settings.galaxyXR.telemetry
    Set-ObjectProperty $telemetry 'enable' $true
    Set-ObjectProperty $telemetry 'listenAddress' $ListenAddress
    Set-ObjectProperty $telemetry 'controlPort' $ControlPort
    Set-ObjectProperty $telemetry 'trackingPort' $TrackingPort
    Set-ObjectProperty $telemetry 'requirePairing' $true
    Set-ObjectProperty $telemetry 'pairingTokenFile' ''
    Set-ObjectProperty $telemetry 'pairingTokenHex' $PairingTokenHex.ToLowerInvariant()
    $allowedClients = @($telemetry.allowedClients | Where-Object { $_.versionCode -ne $VersionCode })
    $allowedClients += [pscustomobject]$record.allowedClient
    Set-ObjectProperty $telemetry 'allowedClients' $allowedClients

    if ($PSCmdlet.ShouldProcess($settingsFullPath, 'Enroll final Galaxy XR APK and configure native telemetry')) {
        Write-Utf8FileAtomically -Path $settingsFullPath -Content ($settings | ConvertTo-Json -Depth 30)
        Write-Host "Galaxy XR telemetry configured: $settingsFullPath"
        Write-Host "Ports: $ControlPort/$TrackingPort (automatic). Exact APK and embedded bridge hashes enrolled."
    }
} else {
    Write-Host 'Run again with -ApplyToSettings to enroll the final APK automatically; no hash or port entry is required.'
}
