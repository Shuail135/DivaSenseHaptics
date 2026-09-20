param(
    [ValidateSet("Release","Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot "build.ps1"
$tempBuild = Join-Path ([System.IO.Path]::GetTempPath()) ("DivaSenseHaptics-package-build-" + $PID)

try {
    & $buildScript -Configuration $Configuration -BuildDirectory $tempBuild
    if ($LASTEXITCODE -ne 0) {
        throw "Build script failed with exit code $LASTEXITCODE."
    }
} finally {
    if (Test-Path $tempBuild) { Remove-Item $tempBuild -Recurse -Force }
}

$dll = Join-Path $root "out\$Configuration\DivaSenseHaptics.dll"
if (-not (Test-Path $dll)) {
    throw "Cannot package because the DLL does not exist: $dll"
}

$dist = Join-Path $root "dist"
$stage = Join-Path $dist "DivaSenseHaptics"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $dll (Join-Path $stage "DivaSenseHaptics.dll") -Force
Copy-Item (Join-Path $root "mod\config.toml") (Join-Path $stage "config.toml") -Force
Copy-Item (Join-Path $root "mod\haptics.ini") (Join-Path $stage "haptics.ini") -Force
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE") -Force
Copy-Item (Join-Path $root "preview.png") (Join-Path $stage "preview.png") -Force

$zip = Join-Path $dist "DivaSenseHaptics-v0.2.0-$Configuration.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force
Write-Host "Package:" $zip
