param(
    [ValidateSet("Release","Debug")]
    [string]$Configuration = "Release",
    [string]$BuildDirectory = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = $BuildDirectory
if ([string]::IsNullOrWhiteSpace($build)) {
    $build = Join-Path ([System.IO.Path]::GetTempPath()) "DivaSenseHaptics-build"
}
$generator = "Visual Studio 17 2022"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake was not found in PATH. Install CMake 3.24+ and reopen Developer PowerShell for Visual Studio 2022."
}

# Always start with a clean temporary build tree. Generated compiler/CMake files
# are intentionally kept outside the source folder.
if (Test-Path $build) {
    Write-Host "Removing previous build directory:" $build
    Remove-Item $build -Recurse -Force
}

Write-Host "Configuring x64 build with $generator..."
& cmake -S $root -B $build -G $generator -A x64 -DDIVASENSE_BUILD_CORE_TESTS=ON
if ($LASTEXITCODE -ne 0) {
    throw @"
CMake configuration failed.

DivaSenseHaptics requires the Visual Studio 2022 C++ toolchain.
Open 'Developer PowerShell for VS 2022' and make sure Visual Studio 2022 / Build Tools has:
  - Desktop development with C++
  - MSVC v143 x64/x86 build tools
  - Windows 10 or Windows 11 SDK

The build script deliberately uses '$generator' because CMake's -A x64 option is supported by Visual Studio generators, not NMake Makefiles.
"@
}

Write-Host "Building $Configuration..."
& cmake --build $build --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE."
}

$smoke = Join-Path $build "$Configuration\DivaSenseCoreSmoke.exe"
if (-not (Test-Path $smoke)) {
    throw "Core smoke-test executable was not produced: $smoke"
}

Write-Host "Running core smoke test..."
& $smoke
if ($LASTEXITCODE -ne 0) {
    throw "Core smoke test failed with exit code $LASTEXITCODE."
}

$dll = Join-Path $build "$Configuration\DivaSenseHaptics.dll"
if (-not (Test-Path $dll)) {
    throw "Build completed but the DLL was not found: $dll"
}

$outDir = Join-Path $root "out\$Configuration"
New-Item -ItemType Directory -Force $outDir | Out-Null
$outDll = Join-Path $outDir "DivaSenseHaptics.dll"
Copy-Item $dll $outDll -Force

Write-Host ""
Write-Host "DLL:" $outDll
