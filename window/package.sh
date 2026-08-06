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
#   - Python embeddable zip downloaded to python/ (for stdlib .pyc)
# =============================================================================
set -e

rm -rf package
mkdir package
cd package

# --- PXView executable and resources ---
cp ../install.dir/bin/PXView.exe .
cp -r ../install.dir/share/PXView/* .
cp -r ../install.dir/share/libsigrokdecode/* .

# --- Resolve MinGW DLL dependencies via ldd ---
# This copies python314.dll, libgcc_s_seh-1.dll, libstdc++-6.dll, etc.
../window/copy-deps.sh PXView.exe /mingw64

# --- Qt6 plugins ---
mkdir -p plugins
cp -r /mingw64/share/qt6/plugins/* .
../window/copy-deps.sh imageformats/qsvg.dll /mingw64
../window/copy-deps.sh imageformats/qjpeg.dll /mingw64

# --- Python standard library ---
# Detect Python version from the python3XX.dll that PXView.exe ACTUALLY links
# (authoritative — avoids mismatch when `python` on PATH is a different version).
PY_VER=""
PY_DLL=$(ldd PXView.exe 2>/dev/null | grep -oE 'libpython3\.[0-9]+\.dll' | head -1)
if [ -n "$PY_DLL" ]; then
    # libpython3.14.dll -> 3.14
    PY_VER=$(echo "$PY_DLL" | grep -oE '3\.[0-9]+')
fi
if [ -z "$PY_VER" ]; then
    # Fallback: extract from /mingw64/bin/python3*.dll filename
    PY_DLL=$(ls /mingw64/bin/python3*.dll 2>/dev/null | head -1)
    if [ -n "$PY_DLL" ]; then
        PY_BASE=$(basename "$PY_DLL" .dll)
        PY_MAJOR="${PY_BASE:6:1}"
        PY_MINOR="${PY_BASE:7}"
        PY_VER="${PY_MAJOR}.${PY_MINOR}"
    fi
fi

if [ -z "$PY_VER" ]; then
    echo "WARNING: Could not detect Python version, skipping stdlib"
else
    echo "Detected MinGW Python version: $PY_VER"

    # Copy the FULL Python standard library from MSYS2 (matches the .pyd
    # extension modules copied below, so libffi/_ctypes stay ABI-compatible).
    # NOTE: Do NOT mix in python.org embeddable zip — its libffi-8.dll is a
    # different build than MinGW's _ctypes.pyd and breaks ctypes import.
    MSYS_PYLIB="/mingw64/lib/python${PY_VER}"
    if [ -d "$MSYS_PYLIB" ]; then
        echo "Copying Python stdlib from MSYS2: $MSYS_PYLIB"
        mkdir -p "lib/python${PY_VER}"
        # Copy everything except the bulky test suite and site-packages junk
        # to keep the package lean while staying self-consistent.
        cp -r "$MSYS_PYLIB"/* "lib/python${PY_VER}/" 2>/dev/null || true
        # Trim the heavyweight test/ idlelib/ turtledemo/ trees (not needed at runtime)
        rm -rf "lib/python${PY_VER}/test" \
               "lib/python${PY_VER}/idlelib" \
               "lib/python${PY_VER}/turtledemo" \
               "lib/python${PY_VER}/__pycache__/test" 2>/dev/null || true
        echo "   [OK] stdlib copied from MSYS2"
    else
        echo "WARNING: $MSYS_PYLIB not found, skipping stdlib"
    fi

    # Copy MinGW's compiled extension modules (.pyd)
    if [ -d "/mingw64/lib/python${PY_VER}/lib-dynload" ]; then
        echo "Copying MinGW Python extension modules (.pyd)"
        cp /mingw64/lib/python${PY_VER}/lib-dynload/*.pyd "lib/python${PY_VER}/" 2>/dev/null || true
    else
        echo "WARNING: /mingw64/lib/python${PY_VER}/lib-dynload not found"
    fi

    # _ctypes.pyd (loaded by ctypes/__init__.py) links against libffi-8.dll,
    # which is NOT a dependency of libpython3.14.dll (so copy-deps.sh won't
    # pick it up). Copy it explicitly from MinGW so it stays ABI-compatible.
    for ffi in /mingw64/bin/libffi-8.dll /mingw64/bin/libffi-7.dll; do
        if [ -f "$ffi" ]; then
            echo "Copying $ffi (required by _ctypes.pyd)"
            cp "$ffi" . 2>/dev/null || true
            break
        fi
    done

    # Copy python3XX._pth if it exists (configures sys.path)
    PY_SHORT=$(echo "$PY_VER" | tr -d '.')
    if [ -f "../python/python${PY_SHORT}._pth" ]; then
        cp "../python/python${PY_SHORT}._pth" .
    fi
fi

# --- Web UI (Vite web client) ---
if [ -d ../web/dist ]; then
    mkdir -p webui
    cp -r ../web/dist/* webui/
fi
