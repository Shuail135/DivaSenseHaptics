param(
    [Parameter(Mandatory=$true)]
    [string]$GameDirectory,
    [ValidateSet("Release","Debug")]
    [string]$Configuration = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root "out\$Configuration\DivaSenseHaptics.dll"
if (!(Test-Path $dll)) { throw "Build the project first: scripts\build.ps1" }

$dest = Join-Path $GameDirectory "mods\DivaSenseHaptics"
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item $dll (Join-Path $dest "DivaSenseHaptics.dll") -Force
Copy-Item (Join-Path $root "mod\config.toml") (Join-Path $dest "config.toml") -Force
Copy-Item (Join-Path $root "mod\haptics.ini") (Join-Path $dest "haptics.ini") -Force
Write-Host "Installed to $dest"
