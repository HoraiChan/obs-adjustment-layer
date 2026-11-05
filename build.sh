#!/bin/bash
set -euo pipefail

# Configuration
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OBS_ROOT="${OBS_ROOT:-$SCRIPT_DIR/../obs-studio}"
DIST_DIR="$SCRIPT_DIR/dist"

echo "Project root: $SCRIPT_DIR"
echo "Using OBS_ROOT: $OBS_ROOT"

# Check for OBS directory
if [ ! -d "$OBS_ROOT/libobs" ]; then
    echo "Error: libobs not found in $OBS_ROOT"
    exit 1
fi

OBS_API_MAJOR=$(sed -n 's/^#define LIBOBS_API_MAJOR_VER //p' "$OBS_ROOT/libobs/obs-config.h")
OBS_API_MINOR=$(sed -n 's/^#define LIBOBS_API_MINOR_VER //p' "$OBS_ROOT/libobs/obs-config.h")
BUILD_DIR="${PLUGIN_BUILD_DIR:-$SCRIPT_DIR/build$OBS_API_MAJOR}"

# Keep older build artifacts; headers and the library must share an OBS build.
if [ -z "${OBS_BUILD_DIR:-}" ]; then
    for candidate in "$OBS_ROOT/build$OBS_API_MAJOR" "$OBS_ROOT/build_macos" "$OBS_ROOT/build"; do
        if [ -f "$candidate/libobs/Release/libobs.framework/libobs" ]; then
            OBS_BUILD_DIR="$candidate"
            break
        fi
    done
fi
OBS_BUILD_DIR="${OBS_BUILD_DIR:?Build OBS libobs first, or set OBS_BUILD_DIR}"

# libobs libraries candidates on Mac
LIBOBS_LIB_CANDIDATES=(
    "$OBS_BUILD_DIR/libobs/Release/libobs.framework/libobs"
    "$OBS_BUILD_DIR/libobs/libobs.dylib"
    "$OBS_BUILD_DIR/libobs/Release/libobs.dylib"
)

LIBOBS_LIB=""
for candidate in "${LIBOBS_LIB_CANDIDATES[@]}"; do
    if [ -f "$candidate" ] || [ -L "$candidate" ]; then
        LIBOBS_LIB="$candidate"
        break
    fi
done

if [ -z "$LIBOBS_LIB" ]; then
    echo "Error: Could not find libobs library in $OBS_BUILD_DIR"
    exit 1
fi

echo "Using LIBOBS_LIB: $LIBOBS_LIB"

# Check the framework's actual version, rather than its folder name.
if [[ "$LIBOBS_LIB" == *.framework/libobs ]]; then
    LIBOBS_VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
        "$(dirname "$LIBOBS_LIB")/Resources/Info.plist")
    if [[ "$LIBOBS_VERSION" != "$OBS_API_MAJOR.$OBS_API_MINOR."* ]]; then
        echo "Error: libobs $LIBOBS_VERSION does not match OBS $OBS_API_MAJOR.$OBS_API_MINOR headers. Rebuild/select matching libobs." >&2
        exit 1
    fi
fi
# Catch stale pre-33 libraries even if their metadata has been altered.
if [ "$OBS_API_MAJOR" -ge 33 ]; then
    if ! nm -gU "$LIBOBS_LIB" | grep ' _gs_image_file_ex_init$' > /dev/null; then
        echo "Error: libobs is older than the OBS 33 headers. Rebuild libobs first." >&2
        exit 1
    fi
fi

cache_value() {
    sed -n "s/^$1:[^=]*=//p" "$OBS_BUILD_DIR/CMakeCache.txt" | head -n 1
}
OBS_ARCH="${OBS_ARCH:-$(cache_value CMAKE_OSX_ARCHITECTURES)}"
OBS_DEPLOYMENT_TARGET="${OBS_DEPLOYMENT_TARGET:-$(cache_value CMAKE_OSX_DEPLOYMENT_TARGET)}"
SIMDE_INCLUDE_DIR=$(cache_value SIMDe_INCLUDE_DIR)

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    "-DLIBOBS_INCLUDE_DIR=$OBS_ROOT/libobs;$OBS_ROOT/libobs/util;$SIMDE_INCLUDE_DIR" \
    "-DLIBOBS_CONFIG_DIR=$OBS_BUILD_DIR/config" \
    "-DLIBOBS_LIB=$LIBOBS_LIB" \
    "-DCMAKE_OSX_ARCHITECTURES=$OBS_ARCH" \
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=$OBS_DEPLOYMENT_TARGET" \
    -DCMAKE_BUILD_TYPE=Release
    
cmake --build "$BUILD_DIR" --config Release --parallel "$(sysctl -n hw.ncpu)"
cmake --install "$BUILD_DIR" --config Release
# Finder metadata copied with artwork is incompatible with code signing.
xattr -cr "$DIST_DIR/adjustment-layer.plugin"
codesign --force --sign - "$DIST_DIR/adjustment-layer.plugin"

echo "--------------------------------------------------"
echo "Build complete (not installed into OBS)."
echo "Artifacts: $DIST_DIR"
