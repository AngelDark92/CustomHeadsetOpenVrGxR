$submodules = @(
    "ThirdParty/openvr",
    "ThirdParty/minhook",
    "ThirdParty/json",
    "ThirdParty/easywsclient",
    "ThirdParty/zlib"
)

foreach ($submodule in $submodules) {
    $moduleName = Split-Path $submodule -Leaf
    $metadataPath = ".git\modules\ThirdParty\$moduleName"
    $worktreePath = $submodule

    Write-Host "`n=== $submodule ==="

    Write-Host "Working directory exists:" (Test-Path $worktreePath)
    Write-Host "Git metadata exists:     " (Test-Path $metadataPath)

    $gitFile = Join-Path $worktreePath ".git"

    if (Test-Path $gitFile) {
        Write-Host ".git contents:"
        Get-Content $gitFile
    }
}