[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')]
    [string[]]$Platform = @('x64', 'Win32'),
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
        throw 'MSBuild was not found on PATH and vswhere.exe is unavailable. Install Visual Studio with Desktop development with C++.'
    }
    $installation = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $installation) { throw 'No Visual Studio installation with the C++ x86/x64 toolset was found.' }
    $candidate = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { throw "MSBuild not found below $installation" }
    return $candidate
}

$msbuild = Resolve-MSBuild

$resourceSource = Join-Path $repo 'GalaxyXRResources\DriverFiles'
$activeOutput = Join-Path $repo 'output\CustomHeadsetOpenVR'
$resourceOutput = Join-Path $repo 'output\galaxyxrresources'
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repo 'output')) + [IO.Path]::DirectorySeparatorChar
foreach ($generatedPackage in @($activeOutput, $resourceOutput)) {
    $resolvedPackage = [IO.Path]::GetFullPath($generatedPackage)
    if (-not $resolvedPackage.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace generated package outside $outputRoot"
    }
    if (Test-Path -LiteralPath $resolvedPackage) {
        Remove-Item -LiteralPath $resolvedPackage -Recurse -Force
    }
}
& (Join-Path $PSScriptRoot 'Test-GalaxyXRResources.ps1')
& (Join-Path $PSScriptRoot 'Test-GalaxyXRResources.ps1') `
    -DriverFiles $resourceSource -ExpectedDriverName 'galaxyxrresources' -ResourceOnly
foreach ($targetPlatform in $Platform) {
    & $msbuild (Join-Path $repo 'CustomHeadsetOpenVR\CustomHeadsetOpenVR.vcxproj') /m $solutionDirArgument /p:Configuration=Release /p:Platform=$targetPlatform /p:ExternalCompilerOptions=/DVENDOR_GALAXYXR /p:DeployToSteamVR=false /p:SkipPostBuild=true
    if ($LASTEXITCODE -ne 0) { throw "Driver $targetPlatform build failed." }
    & $msbuild (Join-Path $repo 'GalaxyXRTests.vcxproj') /m $solutionDirArgument /p:Configuration=Release /p:Platform=$targetPlatform
    if ($LASTEXITCODE -ne 0) { throw "GalaxyXRTests $targetPlatform build failed." }
    & (Join-Path $repo "output\tests\$targetPlatform\Release\GalaxyXRTests.exe")
    if ($LASTEXITCODE -ne 0) { throw "GalaxyXRTests $targetPlatform failed." }
}

$resolvedOutput = [IO.Path]::GetFullPath($resourceOutput)
if (-not $resolvedOutput.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace companion output outside $outputRoot"
}
if (Test-Path -LiteralPath $resolvedOutput) {
    Remove-Item -LiteralPath $resolvedOutput -Recurse -Force
}
Copy-Item -LiteralPath $resourceSource -Destination $resolvedOutput -Recurse
& (Join-Path $PSScriptRoot 'Test-GalaxyXRResources.ps1') `
    -DriverFiles $resolvedOutput -ExpectedDriverName 'galaxyxrresources' -ResourceOnly

$dotnet = Join-Path $repo '.tools\dotnet\dotnet.exe'
if (-not (Test-Path -LiteralPath $dotnet -PathType Leaf)) { $dotnet = 'dotnet' }
& $dotnet build (Join-Path $repo 'VRCFT\GalaxyXR.VRCFaceTracking\GalaxyXR.VRCFaceTracking.csproj') -c Release
if ($LASTEXITCODE -ne 0) { throw 'VRCFT module build failed.' }
$module = Join-Path $repo 'VRCFT\GalaxyXR.VRCFaceTracking\bin\Release\net7.0\GalaxyXR.VRCFaceTracking.dll'
$releaseModule = Join-Path $repo 'output\VRCFT\GalaxyXR.VRCFaceTracking.dll'
New-Item -ItemType Directory -Force -Path (Split-Path $releaseModule) | Out-Null
Copy-Item -LiteralPath $module -Destination $releaseModule -Force

if (-not $SkipGui) {
    Push-Location (Join-Path $repo 'CustomHeadsetGUI')
    $oldVendor = $env:VENDOR
    $oldVendorUi = $env:VENDOR_UI
    try {
        # Keep the active package name vendor-neutral while selecting the
        # Galaxy XR UI/config profile that matches the native driver build.
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

Write-Host 'Galaxy XR static build and test pipeline passed. Nothing was deployed to SteamVR.'
