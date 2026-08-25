[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')]
    [string[]]$Platform = @('x64', 'Win32'),
    [switch]$SkipDriver,
    [switch]$SkipGui
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$solutionDirArgument = "/p:SolutionDir=$repo\"
function Resolve-MSBuild {
    $fromPath = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($fromPath) { return $fromPath.Source }
    $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
    $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'MSBuild was not found. Install Visual Studio with Desktop development with C++.'
    }
    $installation = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $installation) { throw 'No Visual Studio installation with the C++ x86/x64 toolset was found.' }
    $candidate = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { throw "MSBuild not found below $installation" }
    return $candidate
}

$resourceSource = Join-Path $repo 'GalaxyXRResources\DriverFiles'
$activeSource = Join-Path $repo 'CustomHeadsetOpenVR\DriverFiles'
$activeOutput = Join-Path $repo 'output\CustomHeadsetOpenVR'
$resourceOutput = Join-Path $repo 'output\galaxyxrresources'
$obsoleteVrcftOutput = Join-Path $repo 'output\VRCFT'
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repo 'output')) + [IO.Path]::DirectorySeparatorChar
$generatedPackages = @($resourceOutput, $obsoleteVrcftOutput)
if (-not $SkipDriver) {
    $generatedPackages += $activeOutput
}
foreach ($generatedPackage in $generatedPackages) {
    $resolvedPackage = [IO.Path]::GetFullPath($generatedPackage)
    if (-not $resolvedPackage.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace generated package outside $outputRoot"
    }
    if (Test-Path -LiteralPath $resolvedPackage) {
        Remove-Item -LiteralPath $resolvedPackage -Recurse -Force
    }
}

& (Join-Path $PSScriptRoot 'Test-GalaxyXRResources.ps1') `
    -DriverFiles $activeSource -ExpectedDriverName 'CustomHeadsetOpenVR'
& (Join-Path $PSScriptRoot 'Test-GalaxyXRResources.ps1') `
    -DriverFiles $resourceSource -ExpectedDriverName 'galaxyxrresources' -ResourceOnly

if (-not $SkipDriver) {
    $msbuild = Resolve-MSBuild
    foreach ($targetPlatform in $Platform) {
        & $msbuild (Join-Path $repo 'CustomHeadsetOpenVR\CustomHeadsetOpenVR.vcxproj') /m $solutionDirArgument /p:Configuration=Release /p:Platform=$targetPlatform /p:ExternalCompilerOptions=/DVENDOR_GALAXYXR /p:DeployToSteamVR=false /p:SkipPostBuild=true
        if ($LASTEXITCODE -ne 0) { throw "Driver $targetPlatform build failed." }
    }
} elseif (-not (Test-Path -LiteralPath $activeOutput -PathType Container)) {
    throw "-SkipDriver requires an existing staged driver at $activeOutput"
}

Copy-Item -LiteralPath $resourceSource -Destination $resourceOutput -Recurse
& (Join-Path $PSScriptRoot 'Test-GalaxyXRResources.ps1') `
    -DriverFiles $activeOutput -ExpectedDriverName 'CustomHeadsetOpenVR'
& (Join-Path $PSScriptRoot 'Test-GalaxyXRResources.ps1') `
    -DriverFiles $resourceOutput -ExpectedDriverName 'galaxyxrresources' -ResourceOnly

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

Write-Host 'Galaxy XR active driver, resource companion, and GUI validation passed. Nothing was deployed to SteamVR.'
