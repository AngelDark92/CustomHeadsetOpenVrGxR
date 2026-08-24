[CmdletBinding()]
param(
    [switch]$SkipGui
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$resourceSource = Join-Path $repo 'GalaxyXRResources\DriverFiles'
$resourceOutput = Join-Path $repo 'output\galaxyxrresources'
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repo 'output')) + [IO.Path]::DirectorySeparatorChar
$resolvedOutput = [IO.Path]::GetFullPath($resourceOutput)
if (-not $resolvedOutput.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace resource package outside $outputRoot"
}

& (Join-Path $PSScriptRoot 'Test-GalaxyXRResources.ps1') `
    -DriverFiles $resourceSource -ExpectedDriverName 'galaxyxrresources' -ResourceOnly
if (Test-Path -LiteralPath $resolvedOutput) {
    Remove-Item -LiteralPath $resolvedOutput -Recurse -Force
}
$obsoleteOutputs = @(
    [IO.Path]::GetFullPath((Join-Path $repo 'output\CustomHeadsetOpenVR')),
    [IO.Path]::GetFullPath((Join-Path $repo 'output\VRCFT'))
)
foreach ($obsoleteOutput in $obsoleteOutputs) {
    if (-not $obsoleteOutput.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove obsolete package outside $outputRoot"
    }
    if (Test-Path -LiteralPath $obsoleteOutput) {
        Remove-Item -LiteralPath $obsoleteOutput -Recurse -Force
    }
}
Copy-Item -LiteralPath $resourceSource -Destination $resolvedOutput -Recurse
& (Join-Path $PSScriptRoot 'Test-GalaxyXRResources.ps1') `
    -DriverFiles $resolvedOutput -ExpectedDriverName 'galaxyxrresources' -ResourceOnly

if (-not $SkipGui) {
    Push-Location (Join-Path $repo 'CustomHeadsetGUI')
    $oldVendor = $env:VENDOR
    $oldVendorUi = $env:VENDOR_UI
    try {
        $env:VENDOR = ''
        $env:VENDOR_UI = 'galaxyxr'
        npm run build
        if ($LASTEXITCODE -ne 0) { throw 'CustomHeadsetGUI tauri build failed.' }
    } finally {
        $env:VENDOR = $oldVendor
        $env:VENDOR_UI = $oldVendorUi
        Pop-Location
    }
}

Write-Host 'Galaxy XR resource-only build and validation passed. Nothing was deployed to SteamVR.'
