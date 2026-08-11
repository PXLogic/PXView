#!/bin/bash
# full_test.sh - Full E2E test runner for PXView MCP.
#
# Works on both Linux and Windows (MinGW/MSYS2).
# conftest.py uses pxview-automation's PXViewProcess to automatically
# start/stop PXView headless.
#
# Usage:
#   ./tests/full_test.sh                    # auto-find PXView
#   PXVIEW_EXE_PATH=./build/PXView ./tests/full_test.sh   # specify path
#
# Environment variables:
#   PXVIEW_EXE_PATH       Path to PXView executable (default: auto-detect)
#   PXVIEW_STARTUP_TIMEOUT  Seconds to wait for MCP server (default: 120)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Auto-detect PXView executable if not specified
if [ -z "$PXVIEW_EXE_PATH" ]; then
    if [ -f "$PROJECT_ROOT/build/PXView" ]; then
        export PXVIEW_EXE_PATH="$PROJECT_ROOT/build/PXView"
    elif [ -f "$PROJECT_ROOT/build/PXView.exe" ]; then
        export PXVIEW_EXE_PATH="$PROJECT_ROOT/build/PXView.exe"
    elif [ -f "$PROJECT_ROOT/package/PXView.exe" ]; then
        export PXVIEW_EXE_PATH="$PROJECT_ROOT/package/PXView.exe"
    fi
fi

export PXVIEW_STARTUP_TIMEOUT="${PXVIEW_STARTUP_TIMEOUT:-120}"
export PYTHONUNBUFFERED=1

echo "=== Full E2E Test Suite ==="
echo "PXView: ${PXVIEW_EXE_PATH:-auto-detect}"
echo ""

cd "$SCRIPT_DIR"
python -u -m pytest suites/ -v --tb=short \
    --timeout=180 --timeout-method=thread --maxfail=10

echo ""
echo "Done. Exit code: $?"
