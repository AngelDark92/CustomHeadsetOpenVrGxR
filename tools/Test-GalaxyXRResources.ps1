[CmdletBinding()]
param(
    [string]$DriverFiles = '',
    [string]$ExpectedDriverName = 'CustomHeadsetOpenVR',
    [switch]$ResourceOnly
)

$ErrorActionPreference = 'Stop'
if (-not $DriverFiles) {
    $DriverFiles = Join-Path $PSScriptRoot '..\CustomHeadsetOpenVR\DriverFiles'
}
$driverRoot = (Resolve-Path -LiteralPath $DriverFiles).Path
$resources = Join-Path $driverRoot 'resources'
$jsonFiles = Get-ChildItem -LiteralPath $resources -Recurse -File |
    Where-Object { $_.Extension -in @('.json', '.vrsettings') }

foreach ($file in $jsonFiles) {
    $raw = Get-Content -LiteralPath $file.FullName -Raw
    $null = $raw | ConvertFrom-Json
    $rootPattern = '\{' + [regex]::Escape($ExpectedDriverName) + '\}/([^"\\]+(?:/[^"\\]+)*)'
    foreach ($match in [regex]::Matches($raw, $rootPattern)) {
        $relative = $match.Groups[1].Value.Replace('/', [IO.Path]::DirectorySeparatorChar)
        $target = Join-Path $resources $relative
        if (-not (Test-Path -LiteralPath $target)) {
            throw "Missing resource reference in $($file.FullName): $($match.Value)"
        }
    }
    foreach ($match in [regex]::Matches($raw, '"([^/\\"]+\.(?:json|png|obj|mtl|svg|gif))"')) {
        $target = Join-Path $file.DirectoryName $match.Groups[1].Value
        if (-not (Test-Path -LiteralPath $target)) {
            throw "Missing relative resource reference in $($file.FullName): $($match.Groups[1].Value)"
        }
    }
}

foreach ($modelFile in Get-ChildItem -LiteralPath (Join-Path $resources 'rendermodels') -Recurse -File |
    Where-Object { $_.Extension -in @('.obj', '.mtl') }) {
    foreach ($line in Get-Content -LiteralPath $modelFile.FullName) {
        if ($line -match '^\s*(?:mtllib|map_Kd|map_Ks)\s+(.+?)\s*$') {
            $target = Join-Path $modelFile.DirectoryName $Matches[1]
            if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
                throw "Missing model dependency in $($modelFile.FullName): $($Matches[1])"
            }
        }
    }
}

$required = @(
    'driver.vrdrivermanifest',
    'resources\driver.vrresources',
    'resources\input\galaxy_xr_hmd_profile.json',
    'resources\input\galaxy_xr_controller_profile.json',
    'resources\input\vrcompositor_bindings_galaxy_xr_controller.json',
    'resources\icons\galaxyxr\headset_galaxy_xr_status_ready.png',
    'resources\rendermodels\vst_controller_left\vst_controller_left.obj',
    'resources\rendermodels\vst_controller_right\vst_controller_right.obj'
)
if ($ResourceOnly) {
    $required += @(
        'resources\settings\default.vrsettings',
        'resources\rendermodels\galaxy_xr_hmd\galaxy_xr_hmd.obj',
        'resources\rendermodels\galaxy_xr_hmd\galaxy_xr_hmd.mtl',
        'resources\rendermodels\galaxy_xr_hmd\galaxy_xr_hmd.png'
    )
}
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $driverRoot $relative) -PathType Leaf)) {
        throw "Required Galaxy XR package file is missing: $relative"
    }
}

$manifest = Get-Content -LiteralPath (Join-Path $driverRoot 'driver.vrdrivermanifest') -Raw | ConvertFrom-Json
if ($manifest.name -cne $ExpectedDriverName) {
    throw "Driver manifest name '$($manifest.name)' does not match expected '$ExpectedDriverName'."
}
if ($ResourceOnly) {
    if ($manifest.resourceOnly -ne $true -or $manifest.alwaysActivate -ne $true) {
        throw 'GalaxyXRResources must be resourceOnly and alwaysActivate.'
    }
    if ($null -eq $manifest.hmd_presence -or $manifest.hmd_presence.Count -ne 0) {
        throw 'GalaxyXRResources hmd_presence must be an empty array.'
    }
    if (Test-Path -LiteralPath (Join-Path $driverRoot 'bin')) {
        throw 'GalaxyXRResources must not contain a bin directory.'
    }
    $settings = Get-Content -LiteralPath (Join-Path $resources 'settings\default.vrsettings') -Raw | ConvertFrom-Json
    $expectedSections = @(
        'vrlink_xrvst2', 'vrlink_xrvst2_controller_left', 'vrlink_xrvst2_controller_right',
        'vrlink_xrvst2ue', 'vrlink_xrvst2ue_controller_left', 'vrlink_xrvst2ue_controller_right'
    )
    $actualSections = @($settings.PSObject.Properties.Name)
    if ((@($actualSections | Sort-Object) -join "`n") -cne (@($expectedSections | Sort-Object) -join "`n")) {
        throw "GalaxyXRResources settings must contain exactly the six VRLink product/controller sections."
    }
    foreach ($hmdSection in @('vrlink_xrvst2', 'vrlink_xrvst2ue')) {
        $hmd = $settings.$hmdSection
        if ($hmd.serialNumber -cne 'VRLINKHMDGALAXYXR' -or
            $hmd.modelNumber -cne 'Galaxy XR' -or
            $hmd.resourceRoot -cne $ExpectedDriverName -or
            $hmd.renderModelName -cne "{$ExpectedDriverName}/rendermodels/galaxy_xr_hmd") {
            throw "Galaxy identity mismatch in settings section $hmdSection."
        }
    }
} elseif ($manifest.resourceOnly -ne $false) {
    throw 'CustomHeadsetOpenVR must remain the active non-resource-only DLL driver.'
}

Write-Host "Galaxy XR resource graph valid: $($jsonFiles.Count) JSON files parsed."
