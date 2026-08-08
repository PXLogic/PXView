#!/bin/bash
# ============================================================
# Build helper script for sanitizer + clang-tidy configurations
# ============================================================
# Usage:
#   ./build-debug-asan.sh        # ASan + UBSan + Clang-Tidy (recommended for Windows)
#   ./build-debug-asan.sh quick  # ASan only (fastest, catches UAF/overflow)
#   ./build-debug-asan.sh full   # ASan + UBSan + Clang-Tidy + verbose
#
# For TSan (Linux/macOS only):
#   cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DENABLE_UBSAN=ON ..
#   make -j$(nproc)
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build.asan"

MODE="${1:-default}"

CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE=Debug
    -DENABLE_ASAN=ON
    -DENABLE_UBSAN=ON
    -DENABLE_CLANG_TIDY=ON
    -DDISABLE_WERROR=TRUE
)

&)

case "$MODE" in
    quick)
        # ASan only — fastest sanitizer, catches UAF/overflow
        CMAKE_ARGS=(
            -DCMAKE_BUILD_TYPE=Debug
            -DENABLE_ASAN=ON
            -DDISABLE_WERROR=TRUE
        )
        ;;
    full)
        # Everything: ASan + UBSan + Clang-Tidy + verbose
        CMAKE_ARGS=(
            -DCMAKE_BUILD_TYPE=Debug
            -DENABLE_ASAN=ON
            -DENABLE_UBSAN=ON
            -DENABLE_CLANG_TIDY=ON
            -DDISABLE_WERROR=TRUE
            -DCMAKE_VERBOSE_MAKEFILE=ON
        )
        ;;
    default|*)
        # Default: ASan + UBSan + Clang-Tidy
        ;;
esac

echo "============================================================"
echo "  Building with: ${MODE}"
echo "  Build dir:    ${BUILD_DIR}"
echo "  CMake args:   ${CMAKE_ARGS[*]}"
echo "============================================================"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${SCRIPT_DIR}" "${CMAKE_ARGS[@]}"

# Use all available CPU cores
if command -v nproc &>/dev/null; then
    JOBS=$(nproc)
elif command -v sysctl &>/dev/null; then
    JOBS=$(sysctl -n hw.ncpu)
else
    JOBS=4
fi

echo "Building with ${JOBS} threads..."
make -j"${JOBS}"

echo "============================================================"
echo "  Build complete!"
echo "  Run with ASAN_OPTIONS for runtime control:"
echo "    ASAN_OPTIONS=detect_leaks=0 ./build.asan/PXView   # no leak check"
echo "    ASAN_OPTIONS=abort_on_error=1 ./build.asan/PXView  # abort on first error"
echo "    ASAN_OPTIONS=halt_on_error=0 ./build.asan/PXView   # continue after error"
echo "============================================================"
