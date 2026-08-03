#!/bin/bash
# =============================================================================
# package.sh — Windows MinGW dependency bundling for PXView
#
# Run from the repo root (inside MSYS2/MinGW64 shell):
#   bash window/package.sh
#
# Prerequisites:
#   - CMake build + install completed (install.dir/ must exist)
#   - MinGW64 environment active (/mingw64)
# =============================================================================
set -e

rm -rf package
mkdir package
cd package

# --- PXView executable and resources ---
cp ../install.dir/bin/PXView.exe .
cp -r ../install.dir/share/PXView/* .
cp -r ../install.dir/share/libsigrokdecode/* .

# --- Pre-bundled Python embeddable distribution ---
# The python/ directory contains the official Python embeddable zip + DLLs.
cp -r ../python/* .

# --- Resolve MinGW DLL dependencies via ldd ---
../window/copy-deps.sh PXView.exe /mingw64

# --- Qt6 plugins ---
mkdir -p plugins
cp -r /mingw64/share/qt6/plugins/* .
../window/copy-deps.sh imageformats/qsvg.dll /mingw64
../window/copy-deps.sh imageformats/qjpeg.dll /mingw64

# --- Python standard library ---
# Auto-detect Python version from the embeddable zip filename.
# e.g. python314.zip → PY_VER=3.14
PY_ZIP=$(ls python3*.zip 2>/dev/null | head -1)
if [ -z "$PY_ZIP" ]; then
    echo "WARNING: No python3*.zip found, skipping stdlib extraction"
else
    PY_TAG="${PY_ZIP#python}"        # e.g. 314
    PY_TAG="${PY_TAG%.zip}"           # e.g. 314
    PY_MAJOR="${PY_TAG:0:1}"
    PY_MINOR="${PY_TAG:1}"
    PY_VER="${PY_MAJOR}.${PY_MINOR}"  # e.g. 3.14

    echo "Detected Python embeddable: $PY_ZIP (version $PY_VER)"

    # Extract stdlib .pyc files so Python can load modules without zlib
    mkdir -p "lib/python${PY_VER}"
    unzip -q "$PY_ZIP" -d "lib/python${PY_VER}/"

    # Copy MinGW's compiled extension modules (.pyd) if the version matches
    if [ -d "/mingw64/lib/python${PY_VER}/lib-dynload" ]; then
        cp /mingw64/lib/python${PY_VER}/lib-dynload/*.pyd "lib/python${PY_VER}/"
    else
        echo "WARNING: /mingw64/lib/python${PY_VER}/lib-dynload not found"
        echo "  MinGW Python version may differ from embeddable ($PY_VER)."
        echo "  The embeddable's bundled .pyd files will be used instead."
    fi
fi

# --- Web UI (Vite web client) ---
if [ -d ../web/dist ]; then
    mkdir -p webui
    cp -r ../web/dist/* webui/
fi
