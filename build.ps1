$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
$ObsRootInput = if ($env:OBS_ROOT) { $env:OBS_ROOT } else { Join-Path $ScriptDir "../obs-studio" }
$ObsBuildInput = if ($env:OBS_BUILD_DIR) { $env:OBS_BUILD_DIR } else { Join-Path $ObsRootInput "build_x64" }

if (-not (Test-Path -LiteralPath $ObsRootInput -PathType Container)) {
    Write-Error "OBS source directory not found at $ObsRootInput. Set OBS_ROOT to the OBS source directory."
}
if (-not (Test-Path -LiteralPath $ObsBuildInput -PathType Container)) {
    Write-Error "OBS build directory not found at $ObsBuildInput. Set OBS_BUILD_DIR to the selected OBS build directory."
}
$ObsDir = (Resolve-Path -LiteralPath $ObsRootInput).Path
$ObsBuildDir = (Resolve-Path -LiteralPath $ObsBuildInput).Path

Write-Host "Checking OBS Build Directory: $ObsBuildDir"

# OBS 31 vendors SIMD in the source tree. Prefer that copy, then the path
# recorded by the selected OBS build, then the newest matching obs-deps.
$VendoredSimdeInclude = Join-Path $ObsDir "libobs/util"
$SimdeInclude = $null
if (Test-Path -LiteralPath (Join-Path $VendoredSimdeInclude "simde/simde-common.h") -PathType Leaf) {
    $SimdeInclude = $VendoredSimdeInclude
}

$CacheFile = Join-Path $ObsBuildDir "CMakeCache.txt"
$HasSimdeCacheEntry = $false
if (-not $SimdeInclude -and (Test-Path -LiteralPath $CacheFile -PathType Leaf)) {
    $SimdeCacheLine = Get-Content -LiteralPath $CacheFile | Where-Object { $_ -match '^SIMDe_INCLUDE_DIR:[^=]*=' } | Select-Object -First 1
    if ($SimdeCacheLine) {
        $HasSimdeCacheEntry = $true
        $SimdeInclude = ($SimdeCacheLine -replace '^SIMDe_INCLUDE_DIR:[^=]*=', '').Trim()
    }
}
if (-not $SimdeInclude -and -not $HasSimdeCacheEntry) {
    $DepsRoot = Join-Path $ObsDir ".deps"
    $ObsDepsDir = Get-ChildItem -Path $DepsRoot -Filter "obs-deps-*-x64" -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $ObsDepsDir) {
        Write-Error "SIMDe_INCLUDE_DIR is absent from $CacheFile and no obs-deps-*-x64 directory was found under $DepsRoot."
    }
    $SimdeInclude = Join-Path $ObsDepsDir.FullName "include"
}
if (-not $SimdeInclude -or -not (Test-Path -LiteralPath (Join-Path $SimdeInclude "simde/simde-common.h") -PathType Leaf)) {
    Write-Error "SIMDe headers were not found under '$SimdeInclude'. Verify the selected OBS build and its CMakeCache.txt."
}

$LibObsInclude = Join-Path $ObsDir "libobs"
$LibObsConfig = Join-Path $ObsBuildDir "config"
$LibObsConfigHeader = Join-Path $LibObsConfig "obsconfig.h"
$LibObsLib = Join-Path $ObsBuildDir "libobs/Release/obs.lib"
if (-not (Test-Path -LiteralPath $LibObsConfigHeader -PathType Leaf)) {
    Write-Error "OBS config header not found at $LibObsConfigHeader. Build OBS before building the plugin."
}
if (-not (Test-Path -LiteralPath $LibObsLib -PathType Leaf)) {
    Write-Error "Release libobs library not found at $LibObsLib. Build OBS Release x64 before building the plugin."
}

$W32PthreadsDir = Join-Path $ObsDir "deps/w32-pthreads"
Write-Host "Using SIMDe include: $SimdeInclude"

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
$InstallPrefix = $ScriptDir

# CMake Arguments
$CMakeArgs = @(
    "..",
    "-DLIBOBS_INCLUDE_DIR=$LibObsInclude;$LibObsConfig;$SimdeInclude;$W32PthreadsDir",
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
# Keep this plugin's distributable output on the OBS 33+ layout. Remove only
# its own previous package paths; preserve any other artifacts in dist.
$PluginPackage = Join-Path $ScriptDir "dist/plugins/adjustment-layer"
if (Test-Path $PluginPackage) {
    Remove-Item -LiteralPath $PluginPackage -Recurse -Force
}
$LegacyDll = Join-Path $ScriptDir "dist/obs-plugins/64bit/adjustment-layer.dll"
if (Test-Path $LegacyDll) {
    Remove-Item -LiteralPath $LegacyDll -Force
}
$LegacyData = Join-Path $ScriptDir "dist/data/obs-plugins/adjustment-layer"
if (Test-Path $LegacyData) {
    Remove-Item -LiteralPath $LegacyData -Recurse -Force
}

& $CMakeExe --install . --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Build Complete!"
Write-Host "Artifacts are in: (Project Root)/dist"
