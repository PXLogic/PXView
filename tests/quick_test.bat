@echo off
setlocal

echo Starting PXView...
cd /d "c:\Users\admin\Downloads\Downloads\DSView-main_2026_4_27cppnb\package"
start /B "" PXView.exe --headless -l 4 > "%TEMP%\pxview_headless.log" 2>&1

echo Waiting 90 seconds for MCP server to be ready...
timeout /t 90 /nobreak

echo Checking MCP server...
curl.exe -s -X POST http://127.0.0.1:10110/mcp -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}" --max-time 5

if %errorlevel% equ 0 (
    echo MCP server is ready!
    echo.
    echo Running tests...
    cd /d "c:\Users\admin\Downloads\Downloads\DSView-main_2026_4_27cppnb\tests"
    python -m pytest suites/test_01_mcp_protocol.py -v --tb=short --timeout=120 --timeout-method=thread
) else (
    echo ERROR: MCP server not ready
    type "%TEMP%\pxview_headless.log"
)

echo.
echo Stopping PXView...
taskkill /F /IM PXView.exe

endlocal