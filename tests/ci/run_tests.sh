#!/bin/bash
# ci/run_tests.sh - Linux 本地 E2E 测试脚本
#
# 用法：
#   1. 先在本地编译并打包（install.dir/ 目录存在）
#   2. 运行此脚本：
#      ./tests/ci/run_tests.sh --package-dir ./install.dir/bin/
#
# 参数：
#   --package-dir DIR   PXView 可执行文件所在目录，必填
#   --mcp-url URL       MCP 服务地址，默认 http://127.0.0.1:10110/mcp
#   --timeout SECS      等待 MCP 启动的超时秒数，默认 90
#   --pytest-args ARGS  额外的 pytest 参数

set -e

# ---- 默认值 ----
PACKAGE_DIR=""
MCP_URL="http://127.0.0.1:10110/mcp"
STARTUP_TIMEOUT=90
PYTEST_ARGS=""

# ---- 解析参数 ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --package-dir)
            PACKAGE_DIR="$2"
            shift 2
            ;;
        --mcp-url)
            MCP_URL="$2"
            shift 2
            ;;
        --timeout)
            STARTUP_TIMEOUT="$2"
            shift 2
            ;;
        --pytest-args)
            PYTEST_ARGS="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 --package-dir DIR [--mcp-url URL] [--timeout SECS] [--pytest-args ARGS]"
            exit 1
            ;;
    esac
done

if [ -z "$PACKAGE_DIR" ]; then
    echo "ERROR: --package-dir is required"
    echo "Usage: $0 --package-dir DIR [--mcp-url URL] [--timeout SECS] [--pytest-args ARGS]"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TESTS_DIR="$PROJECT_ROOT/tests"

# ---- 解析绝对路径 ----
PKG_DIR="$(cd "$PACKAGE_DIR" && pwd)"
PXV_BIN="$PKG_DIR/PXView"

# ============================================================
# 1. 验证 PXView 可执行文件
# ============================================================
echo ""
echo "========================================"
echo "  PXView E2E Test Runner (Local)"
echo "========================================"
echo ""
echo "Package dir:  $PKG_DIR"
echo "Tests dir:    $TESTS_DIR"
echo "MCP URL:      $MCP_URL"
echo ""

if [ ! -f "$PXV_BIN" ]; then
    echo "ERROR: PXView not found at $PXV_BIN"
    echo "Make sure you have built PXView first."
    echo "  1. cmake --build build"
    echo "  2. ./tests/ci/run_tests.sh --package-dir ./install.dir/bin/"
    exit 1
fi

echo "PXView:       $PXV_BIN"

# ============================================================
# 2. 安装 Python 测试依赖
# ============================================================
echo ""
echo "=== Checking Python test dependencies ==="
python3 -c "import pytest" 2>/dev/null || {
    echo "Installing pytest, pytest-html, requests..."
    python3 -m pip install --quiet pytest pytest-html requests
} && echo "All dependencies already installed."

# ============================================================
# 3. 启动 PXView headless
# ============================================================
echo ""
echo "=== Starting PXView in headless mode ==="

LOG_FILE="/tmp/pxview_headless_$$.log"

"$PXV_BIN" --headless -l 4 > "$LOG_FILE" 2>&1 &
PXV_PID=$!
echo "PXView PID: $PXV_PID"

# 确保退出时停止 PXView
cleanup() {
    echo ""
    echo "=== Stopping PXView ==="
    kill $PXV_PID 2>/dev/null || true
    wait $PXV_PID 2>/dev/null || true
    echo ""
    echo "=== PXView headless log (last 30 lines) ==="
    tail -30 "$LOG_FILE" 2>/dev/null || true
    rm -f "$LOG_FILE"
}
trap cleanup EXIT

# ============================================================
# 4. 等待 MCP 服务就绪
# ============================================================
echo ""
echo "=== Waiting for MCP server to be ready ==="
READY=0

for i in $(seq 1 $STARTUP_TIMEOUT); do
    if ! kill -0 $PXV_PID 2>/dev/null; then
        echo ""
        echo "ERROR: PXView exited prematurely"
        echo ""
        echo "=== PXView log ==="
        cat "$LOG_FILE"
        exit 1
    fi

    if curl -s -X POST "$MCP_URL" \
        -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"ci","version":"1.0"}}}' \
        2>/dev/null | grep -q "protocolVersion"; then
        echo "MCP server ready after ${i}s"
        READY=1
        break
    fi

    if [ $((i % 10)) -eq 0 ]; then
        echo "  Still waiting... (${i}s elapsed)"
    fi
    sleep 1
done

if [ $READY -eq 0 ]; then
    echo ""
    echo "ERROR: MCP server did not start within ${STARTUP_TIMEOUT}s"
    echo ""
    echo "=== PXView log (last 80 lines) ==="
    tail -80 "$LOG_FILE" 2>/dev/null
    exit 1
fi

# ============================================================
# 5. 运行 pytest E2E 测试
# ============================================================
echo ""
echo "=== Running pytest E2E test suite ==="

cd "$TESTS_DIR"
export PXVIEW_MCP_URL="$MCP_URL"
export PXVIEW_STARTUP_TIMEOUT="30"

HTML_REPORT="$PROJECT_ROOT/test_report.html"
XML_REPORT="$PROJECT_ROOT/test_report.xml"

TEST_EXIT=0
python3 -m pytest suites/ -v --tb=short \
    --html="$HTML_REPORT" \
    --junitxml="$XML_REPORT" \
    --self-contained-html \
    $PYTEST_ARGS || TEST_EXIT=$?

# ============================================================
# 6. 汇总结果
# ============================================================
echo ""
echo "========================================"
echo "  Test Results"
echo "========================================"

if [ $TEST_EXIT -eq 0 ]; then
    echo "  ALL TESTS PASSED"
else
    echo "  SOME TESTS FAILED (exit code: $TEST_EXIT)"
fi

echo ""
echo "  HTML report: $HTML_REPORT"
echo "  JUnit XML:   $XML_REPORT"
echo ""

exit $TEST_EXIT
