$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
$ObsDir = Resolve-Path "$ScriptDir/../obs-studio"
$ObsBuildDir = "$ObsDir/build_x64"

Write-Host "Checking OBS Build Directory: $ObsBuildDir"

if (-not (Test-Path $ObsBuildDir)) {
    Write-Error "OBS Studio build directory not found at $ObsBuildDir. Please build obs-studio first."
}

# Find obs-deps directory for SIMD headers
$ObsDepsDir = Get-ChildItem -Path "$ObsDir/.deps" -Filter "obs-deps-*-x64" | Select-Object -First 1
if (-not $ObsDepsDir) {
    Write-Error "Could not find obs-deps directory in $ObsDir/.deps"
}
$ObsDepsInclude = Join-Path $ObsDepsDir.FullName "include"
$W32PthreadsDir = Join-Path $ObsDir "deps/w32-pthreads"

Write-Host "Found obs-deps include: $ObsDepsInclude"

$CMakeExe = "cmake"
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    $CommonCMakePaths = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\CMake\bin\cmake.exe"
    )
    foreach ($Path in $CommonCMakePaths) {
        if (Test-Path $Path) {
            $CMakeExe = $Path
            break
        }
    }
}
Write-Host "Using CMake: $CMakeExe"

$BuildDir = Join-Path $ScriptDir "build"
if (Test-Path $BuildDir) {
    Remove-Item -Path $BuildDir -Recurse -Force
}
New-Item -Path $BuildDir -ItemType Directory | Out-Null

Set-Location $BuildDir

# Paths for CMake
$LibObsInclude = "$($ObsDir.path)/libobs"
$LibObsConfig = "$ObsBuildDir/config"
$LibObsLib = "$ObsBuildDir/libobs/Release/obs.lib"
$InstallPrefix = $ScriptDir

# CMake Arguments
$CMakeArgs = @(
    "..",
    "-DLIBOBS_INCLUDE_DIR=$LibObsInclude;$LibObsConfig;$ObsDepsInclude;$W32PthreadsDir",
    "-DLIBOBS_CONFIG_DIR=$LibObsConfig",
    "-DLIBOBS_LIB=$LibObsLib",
    "-DCMAKE_INSTALL_PREFIX=$InstallPrefix",
    "-DCMAKE_BUILD_TYPE=Release",
    "-A", "x64"
)

Write-Host "Configuring..."
& $CMakeExe $CMakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Building..."
& $CMakeExe --build . --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Installing..."
& $CMakeExe --install . --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Build Complete!"
Write-Host "Artifacts are in: (Project Root)/dist"
