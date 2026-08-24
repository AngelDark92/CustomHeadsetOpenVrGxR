[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')]
    [string[]]$Platform = @('x64', 'Win32'),
    [switch]$SkipGui
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$solutionDirArgument = "/p:SolutionDir=$repo\"
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
    throw "MSBuild not found at $msbuild. Run from a Visual Studio developer shell or update the script path."
}

$resourceSource = Join-Path $repo 'GalaxyXRResources\DriverFiles'
$resourceOutput = Join-Path $repo 'output\galaxyxrresources'
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
$expectedOutputRoot = [IO.Path]::GetFullPath((Join-Path $repo 'output')) + [IO.Path]::DirectorySeparatorChar
if (-not $resolvedOutput.StartsWith($expectedOutputRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace companion output outside $expectedOutputRoot"
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
    try {
        npm run build
        if ($LASTEXITCODE -ne 0) { throw 'CustomHeadsetGUI tauri build failed.' }
    } finally { Pop-Location }
}

Write-Host 'Galaxy XR static build and test pipeline passed. Nothing was deployed to SteamVR.'
