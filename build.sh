#!/bin/bash
set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OBS_ROOT="${OBS_ROOT:-$SCRIPT_DIR/../obs-studio}"
BUILD_DIR="$SCRIPT_DIR/build"
DIST_DIR="$SCRIPT_DIR/dist"

echo "Project root: $SCRIPT_DIR"
echo "Using OBS_ROOT: $OBS_ROOT"

# Check for OBS directory
if [ ! -d "$OBS_ROOT/libobs" ]; then
    echo "Error: libobs not found in $OBS_ROOT"
    exit 1
fi

# Clean up previous build/dist
rm -rf "$BUILD_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$BUILD_DIR"

cd "$BUILD_DIR"

echo "Running build for macOS..."

# libobs libraries candidates on Mac
LIBOBS_LIB_CANDIDATES=(
    "$OBS_ROOT/build/libobs/Release/libobs.framework/libobs"
    "$OBS_ROOT/build/libobs/libobs.dylib"
    "$OBS_ROOT/build/libobs/Release/libobs.dylib"
)

LIBOBS_LIB=""
for candidate in "${LIBOBS_LIB_CANDIDATES[@]}"; do
    if [ -f "$candidate" ] || [ -L "$candidate" ]; then
        LIBOBS_LIB="$candidate"
        break
    fi
done

if [ -z "$LIBOBS_LIB" ]; then
    echo "Error: Could not find libobs library in $OBS_ROOT/build"
    exit 1
fi

echo "Using LIBOBS_LIB: $LIBOBS_LIB"

# CMake
cmake .. \
    -DLIBOBS_INCLUDE_DIR="$OBS_ROOT/libobs" \
    -DLIBOBS_LIB="$LIBOBS_LIB" \
    -DCMAKE_BUILD_TYPE=Release
    
make -j$(sysctl -n hw.ncpu)
make install

echo "--------------------------------------------------"
echo "Build and Install complete!"
echo "Artifacts: $DIST_DIR"
