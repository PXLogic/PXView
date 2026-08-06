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
# This copies libpython3.14.dll, libgcc_s_seh-1.dll, libstdc++-6.dll, etc.
../window/copy-deps.sh PXView.exe /mingw64

# --- Explicitly copy MinGW Python DLLs as a safety net ---
# copy-deps.sh relies on ldd, which may not resolve all dependencies
# correctly in CI environments. Explicitly copy the MinGW Python DLLs
# so they are always present.
# NOTE: Do NOT copy python.org embeddable DLLs (python314.dll, python3.dll).
# They are MSVC-built and ABI-incompatible with MinGW's .pyd extension
# modules. If PXView.exe links against python314.dll (python.org naming)
# instead of libpython3.14.dll (MinGW naming), it means CMake found the
# wrong Python — fix the root cause in CMake (CMP0148), don't paper over
# it by mixing two incompatible Python runtimes in one process.
for py_dll in /mingw64/bin/libpython3.*.dll /mingw64/bin/libpython3.dll; do
    if [ -f "$py_dll" ]; then
        dll_name=$(basename "$py_dll")
        if [ ! -f "$dll_name" ]; then
            echo "Explicitly copying Python DLL: $py_dll"
            cp "$py_dll" .
        fi
    fi
done

# --- Detect which Python build PXView.exe links against ---
# This is a diagnostic: if PXView links against python3XX.dll (python.org
# naming), the build is misconfigured and the package will not work.
PY_LINK=$(ldd PXView.exe 2>/dev/null | grep -oE '(lib)?python3[0-9.]*\.dll' | head -1)
if echo "$PY_LINK" | grep -q '^python3'; then
    echo "============================================================"
    echo "ERROR: PXView.exe links against $PY_LINK (python.org MSVC build)"
    echo "       Expected: libpython3.XX.dll (MinGW GCC build)"
    echo "       CMake found the wrong Python. Fix: ensure CMake uses"
    echo "       CMP0148 OLD on Windows to search PATH (MSYS2 MinGW)"
    echo "       instead of the Windows registry (python.org)."
    echo "       The .pyd extensions from MSYS2 require libpython3.XX.dll"
    echo "       and will crash if loaded against python3XX.dll."
    echo "============================================================"
    exit 1
fi

# --- Qt6 plugins ---
mkdir -p plugins
cp -r /mingw64/share/qt6/plugins/* .
../window/copy-deps.sh imageformats/qsvg.dll /mingw64
../window/copy-deps.sh imageformats/qjpeg.dll /mingw64

# --- Python standard library ---
# Detect Python version from the DLL that PXView.exe ACTUALLY links.
# Try both MinGW naming (libpython3.X.dll) and python.org naming (python3XX.dll).
PY_VER=""
PY_DLL=$(ldd PXView.exe 2>/dev/null | grep -oE 'libpython3\.[0-9]+\.dll' | head -1)
if [ -n "$PY_DLL" ]; then
    # libpython3.14.dll -> 3.14
    PY_VER=$(echo "$PY_DLL" | grep -oE '3\.[0-9]+')
fi
if [ -z "$PY_VER" ]; then
    # Try python.org naming: python314.dll
    PY_DLL=$(ldd PXView.exe 2>/dev/null | grep -oE 'python3[0-9]+\.dll' | head -1)
    if [ -n "$PY_DLL" ]; then
        # python314.dll -> 3.14
        PY_TAG=$(echo "$PY_DLL" | grep -oE '3[0-9]+')
        PY_MAJOR="${PY_TAG:0:1}"
        PY_MINOR="${PY_TAG:1}"
        PY_VER="${PY_MAJOR}.${PY_MINOR}"
    fi
fi
if [ -z "$PY_VER" ]; then
    # Fallback 1: extract from /mingw64/bin/libpython3*.dll filename
    PY_DLL=$(ls /mingw64/bin/libpython3.*.dll 2>/dev/null | head -1)
    if [ -n "$PY_DLL" ]; then
        PY_VER=$(basename "$PY_DLL" .dll | grep -oE '3\.[0-9]+')
    fi
fi
if [ -z "$PY_VER" ]; then
    # Fallback 2: use the MSYS2 python interpreter version
    PY_VER=$(python -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")' 2>/dev/null || echo "")
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

    # Copy python3XX.zip from embeddable package. This serves two purposes:
    # 1. appcontrol.cpp checks for python*.zip to detect bundled Python and
    #    set PYTHONHOME to the application directory.
    # 2. The .pyc files inside are bytecode-compatible with MinGW Python.
    # NOTE: Do NOT copy python3XX._pth — it forces Python into isolated mode,
    # restricting sys.path to only the paths listed in the _pth file
    # (python314.zip and .). This conflicts with the MSYS2 MinGW stdlib
    # layout (lib/python3.14/). Without the _pth file, Python uses
    # PYTHONHOME-based sys.path, which correctly finds lib/python3.14/.
    PY_SHORT=$(echo "$PY_VER" | tr -d '.')
    if [ -f "../python/python${PY_SHORT}.zip" ]; then
        echo "Copying python${PY_SHORT}.zip (for PYTHONHOME detection + .pyc stdlib)"
        cp "../python/python${PY_SHORT}.zip" .
    fi
fi

# --- Web UI (Vite web client) ---
if [ -d ../web/dist ]; then
    mkdir -p webui
    cp -r ../web/dist/* webui/
fi
