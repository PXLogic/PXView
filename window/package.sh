#!/bin/bash
# =============================================================================
# package.sh — Windows MinGW dependency bundling for PXView
#
# Run from the repo root (inside MSYS2/UCRT64 shell):
#   bash window/package.sh
#
# Prerequisites:
#   - CMake build + install completed (install.dir/ must exist)
#   - UCRT64 environment active (/ucrt64)
#   - Python installed in UCRT64 (mingw-w64-python provides stdlib + .pyd)
# =============================================================================
set -e

echo "=== package.sh start ==="
echo "Working directory: $(pwd)"
echo "MSYSTEM: ${MSYSTEM:-not set}"

rm -rf package
mkdir package
cd package

# --- PXView executable and resources ---
cp ../install.dir/bin/PXView.exe .
cp -r ../install.dir/share/PXView/* .
cp -r ../install.dir/share/libsigrokdecode/* .

echo "=== Copying MinGW DLL dependencies ==="
# --- Resolve MinGW DLL dependencies via ldd ---
# This copies libpython3.14.dll, libgcc_s_seh-1.dll, libstdc++-6.dll, etc.
../window/copy-deps.sh PXView.exe /ucrt64

# --- Qt6 plugins ---
mkdir -p plugins
cp -r /ucrt64/share/qt6/plugins/* .
../window/copy-deps.sh imageformats/qsvg.dll /ucrt64
../window/copy-deps.sh imageformats/qjpeg.dll /ucrt64

# --- Python standard library ---
# Detect Python version from the Python DLL that PXView.exe ACTUALLY links.
# With the deps.cmake fix, PXView.exe links against libpython3.XX.dll (MinGW
# naming). We also check python3XX.dll (python.org naming) as a diagnostic —
# if that's found, it means CMake found the wrong Python and the build is broken.
echo "=== Python version detection ==="

# Diagnostic: check what Python DLL PXView.exe actually links against
PY_LINK_RAW=$(ldd PXView.exe 2>/dev/null | grep -iE '(lib)?python3[0-9._]*\.dll' || true)
echo "PXView.exe Python DLL dependency (ldd):"
echo "$PY_LINK_RAW" | sed 's/^/  /'

PY_VER=""
PY_DLL=""

# Method 1: ldd shows libpython3.XX.dll (MinGW naming — correct)
PY_DLL=$(echo "$PY_LINK_RAW" | grep -oE 'libpython3\.[0-9]+\.dll' | head -1)
if [ -n "$PY_DLL" ]; then
    # libpython3.14.dll -> 3.14
    PY_VER=$(echo "$PY_DLL" | grep -oE '3\.[0-9]+')
    echo "Detected MinGW Python (libpython naming): $PY_DLL -> version $PY_VER"
fi

# Method 2: ldd shows python3XX.dll (python.org MSVC naming — WRONG)
if [ -z "$PY_VER" ]; then
    PY_DLL_MSVC=$(echo "$PY_LINK_RAW" | grep -oE 'python3[0-9]+\.dll' | head -1)
    if [ -n "$PY_DLL_MSVC" ]; then
        echo "ERROR: PXView.exe links against $PY_DLL_MSVC (python.org MSVC build)"
        echo "       This DLL is NOT available at runtime."
        echo "       CMake found the wrong Python. Check deps.cmake MSYS2 detection."
        # Extract version as best-effort: python314.dll -> 3.14
        PY_SHORT=$(echo "$PY_DLL_MSVC" | grep -oE '[0-9]+' | head -1)
        if [ ${#PY_SHORT} -ge 3 ]; then
            PY_VER="${PY_SHORT:0:1}.${PY_SHORT:1}"
            echo "       Extracted version: $PY_VER (using for stdlib copy as fallback)"
        fi
    fi
fi

# Method 3: Fallback — find libpython3.XX.dll in /ucrt64/bin
if [ -z "$PY_VER" ]; then
echo "ldd did not find Python DLL, trying /ucrt64/bin/..."
PY_DLL=$(ls /ucrt64/bin/libpython3*.dll 2>/dev/null | head -1)
    if [ -n "$PY_DLL" ]; then
        PY_VER=$(echo "$(basename "$PY_DLL")" | grep -oE '3\.[0-9]+')
        echo "Found MinGW Python DLL in /ucrt64/bin: $(basename "$PY_DLL") -> version $PY_VER"
    fi
fi

# Method 4: Final fallback — query the python interpreter directly
if [ -z "$PY_VER" ]; then
    echo "Trying python interpreter..."
    PY_VER=$(python -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")' 2>/dev/null || true)
    if [ -n "$PY_VER" ]; then
        echo "Detected Python version from interpreter: $PY_VER"
    fi
fi

if [ -z "$PY_VER" ]; then
    echo "WARNING: Could not detect Python version, skipping stdlib"
else
    echo "=== Python version: $PY_VER ==="

    # Copy the FULL Python standard library from MSYS2 (matches the .pyd
    # extension modules copied below, so libffi/_ctypes stay ABI-compatible).
    # NOTE: Do NOT mix in python.org embeddable zip — its libffi-8.dll is a
    # different build than MinGW's _ctypes.pyd and breaks ctypes import.
    MSYS_PYLIB="/ucrt64/lib/python${PY_VER}"
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

        # Verify encodings module exists (critical for Python startup)
        if [ -d "lib/python${PY_VER}/encodings" ]; then
            echo "   [OK] encodings module found"
        else
            echo "   ERROR: encodings module NOT found in lib/python${PY_VER}/"
            echo "   Python will fail with: ModuleNotFoundError: No module named 'encodings'"
        fi
    else
        echo "WARNING: $MSYS_PYLIB not found, skipping stdlib"
    fi

    # Copy MinGW's compiled extension modules (.pyd)
    if [ -d "/ucrt64/lib/python${PY_VER}/lib-dynload" ]; then
        echo "Copying MinGW Python extension modules (.pyd)"
        cp /ucrt64/lib/python${PY_VER}/lib-dynload/*.pyd "lib/python${PY_VER}/" 2>/dev/null || true
        echo "   [OK] .pyd files copied"
    else
        echo "WARNING: /ucrt64/lib/python${PY_VER}/lib-dynload not found"
    fi

    # _ctypes.pyd (loaded by ctypes/__init__.py) links against libffi-8.dll,
    # which is NOT a dependency of libpython3.14.dll (so copy-deps.sh won't
    # pick it up). Copy it explicitly from MinGW so it stays ABI-compatible.
    for ffi in /ucrt64/bin/libffi-8.dll /ucrt64/bin/libffi-7.dll; do
        if [ -f "$ffi" ]; then
            echo "Copying $ffi (required by _ctypes.pyd)"
            cp "$ffi" . 2>/dev/null || true
            break
        fi
    done

    # Copy python3XX._pth if it exists (configures sys.path)
    # NOTE: We deliberately do NOT copy python3XX._pth because it activates
    # Python's isolated mode, which prevents finding lib/pythonX.Y/ stdlib.
    # The appcontrol.cpp sets PYTHONHOME instead, which is the correct approach.
    PY_SHORT=$(echo "$PY_VER" | tr -d '.')
    PTH_FILE="../python/python${PY_SHORT}._pth"
    if [ -f "$PTH_FILE" ]; then
        echo "NOTE: $PTH_FILE exists but NOT copying it (would activate isolated mode)"
    fi
fi

# --- Web UI (Vite web client) ---
if [ -d ../web/dist ]; then
    mkdir -p webui
    cp -r ../web/dist/* webui/
fi

# --- Summary ---
echo "=== package.sh summary ==="
echo "DLLs in package: $(ls *.dll 2>/dev/null | wc -l)"
echo "Python DLLs: $(ls *python*.*.dll 2>/dev/null || echo 'NONE')"
if [ -n "$PY_VER" ] && [ -d "lib/python${PY_VER}" ]; then
    echo "Python stdlib: lib/python${PY_VER}/ ($(du -sh lib/python${PY_VER}/ 2>/dev/null | awk '{print $1}'))"
else
    echo "Python stdlib: NOT COPIED"
fi
echo "=== package.sh done ==="
