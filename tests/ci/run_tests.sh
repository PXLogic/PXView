#!/bin/bash
# ci/run_tests.sh - Cross-platform E2E test runner for PXView MCP.
#
# Works on both Linux and Windows (MinGW/MSYS2).
# conftest.py uses pxview-automation's PXViewProcess to automatically
# start/stop PXView headless.
#
# Usage:
#   ./tests/ci/run_tests.sh                          # auto-detect PXView
#   ./tests/ci/run_tests.sh --exe ./build/PXView     # specify exe path
#   ./tests/ci/run_tests.sh --exe ./build/PXView.exe # Windows
#
# Options:
#   --exe PATH          Path to PXView executable
#   --pytest-args ARGS  Extra pytest arguments
#   --timeout SECS      MCP startup timeout (default: 120)

set -e

# ---- Defaults ----
EXE_PATH=""
PYTEST_ARGS=""
STARTUP_TIMEOUT=120

# ---- Parse arguments ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --exe)
            EXE_PATH="$2"
            shift 2
            ;;
        --pytest-args)
            PYTEST_ARGS="$2"
            shift 2
            ;;
        --timeout)
            STARTUP_TIMEOUT="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 [--exe PATH] [--pytest-args ARGS] [--timeout SECS]"
            exit 1
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TESTS_DIR="$PROJECT_ROOT/tests"

# ---- Auto-detect PXView executable if not specified ----
if [ -z "$EXE_PATH" ]; then
    if [ -f "$PROJECT_ROOT/build/PXView" ]; then
        EXE_PATH="$PROJECT_ROOT/build/PXView"
    elif [ -f "$PROJECT_ROOT/build/PXView.exe" ]; then
        EXE_PATH="$PROJECT_ROOT/build/PXView.exe"
    elif [ -f "$PROJECT_ROOT/package/PXView.exe" ]; then
        EXE_PATH="$PROJECT_ROOT/package/PXView.exe"
    elif [ -f "$PROJECT_ROOT/package/PXView" ]; then
        EXE_PATH="$PROJECT_ROOT/package/PXView"
    fi
fi

echo ""
echo "========================================"
echo "  PXView E2E Test Runner"
echo "========================================"
echo ""
echo "PXView:      ${EXE_PATH:-auto-detect}"
echo "Tests dir:   $TESTS_DIR"
echo "Timeout:     ${STARTUP_TIMEOUT}s"
echo ""

# ---- Install Python test dependencies ----
echo "=== Checking Python test dependencies ==="
python3 -c "import pytest" 2>/dev/null || {
    echo "Installing pytest, pytest-html, pytest-timeout..."
    python3 -m pip install --quiet pytest pytest-html pytest-timeout
} && echo "pytest OK"

# Install pxview-automation from source if not installed
python3 -c "import pxview_automation" 2>/dev/null || {
    echo "Installing pxview-automation..."
    python3 -m pip install -e "$PROJECT_ROOT/pxview-automation"
} && echo "pxview-automation OK"

# ---- Run pytest E2E tests ----
echo ""
echo "=== Running pytest E2E test suite ==="
echo "(conftest.py will auto-start PXView headless via PXViewProcess)"

cd "$TESTS_DIR"
export PXVIEW_EXE_PATH="$EXE_PATH"
export PXVIEW_STARTUP_TIMEOUT="$STARTUP_TIMEOUT"
export PXVIEW_MCP_URL="http://127.0.0.1:10110/mcp"

HTML_REPORT="$PROJECT_ROOT/test_report.html"
XML_REPORT="$PROJECT_ROOT/test_report.xml"

TEST_EXIT=0
python3 -m pytest suites/ -v --tb=short \
    --html="$HTML_REPORT" \
    --junitxml="$XML_REPORT" \
    --self-contained-html \
    --timeout=300 --timeout-method=thread \
    $PYTEST_ARGS || TEST_EXIT=$?

# ---- Cleanup ----
pkill -f "PXView.*--headless" 2>/dev/null || true

# ---- Summary ----
echo ""
echo "========================================"
echo "  Test Results"
echo "========================================"

if [ "$TEST_EXIT" -eq 0 ]; then
    echo "  ALL TESTS PASSED"
else
    echo "  SOME TESTS FAILED (exit code: $TEST_EXIT)"
fi

echo ""
echo "  HTML report: $HTML_REPORT"
echo "  JUnit XML:   $XML_REPORT"
echo ""

exit $TEST_EXIT
