[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string]$ApkPath,
    [ValidateScript({ $_ -eq 0 -or $_ -in @(5002318, 5002322) })]
    [int]$VersionCode = 0,
    [string]$ListenAddress = '',
    [string]$PairingTokenHex = '',
    [string]$ApkAnalyzerPath = '',
    [string]$SettingsPath = ''
)

$arguments = @{
    ApkPath = $ApkPath
    ApplyToSettings = $true
    Confirm = $false
}
if ($VersionCode) { $arguments.VersionCode = $VersionCode }
if ($ListenAddress) { $arguments.ListenAddress = $ListenAddress }
if ($PairingTokenHex) { $arguments.PairingTokenHex = $PairingTokenHex }
if ($ApkAnalyzerPath) { $arguments.ApkAnalyzerPath = $ApkAnalyzerPath }
if ($SettingsPath) { $arguments.SettingsPath = $SettingsPath }
if ($WhatIfPreference) { $arguments.WhatIf = $true }

& (Join-Path $PSScriptRoot 'New-GalaxyXRAdmissionReport.ps1') @arguments
