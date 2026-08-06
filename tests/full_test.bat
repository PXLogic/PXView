@echo off
setlocal

echo Starting PXView...
cd /d "c:\Users\admin\Downloads\Downloads\DSView-main_2026_4_27cppnb\package"
start /B "" PXView.exe --headless -l 4 > "%TEMP%\pxview_headless.log" 2>&1

echo Waiting 90 seconds for MCP server to be ready...
timeout /t 90 /nobreak

echo Checking MCP server...
curl.exe -s -X POST http://127.0.0.1:10110/mcp -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}" --max-time 5
if %errorlevel% neq 0 (
    echo ERROR: MCP server not ready
    type "%TEMP%\pxview_headless.log"
    taskkill /F /IM PXView.exe
    exit /b 1
)

echo MCP server is ready!
echo.
echo Running full test suite...
cd /d "c:\Users\admin\Downloads\Downloads\DSView-main_2026_4_27cppnb\tests"
set PYTHONUNBUFFERED=1
python -u -m pytest suites/ -v --tb=short --timeout=120 --timeout-method=thread --maxfail=10 > "%TEMP%\test_full_output.txt" 2>&1

echo.
echo Test results saved to %TEMP%\test_full_output.txt
type "%TEMP%\test_full_output.txt" | findstr /C:"passed" /C:"failed" /C:"error" /C:"====" /C:"ERRORS"

echo.
echo Stopping PXView...
taskkill /F /IM PXView.exe

endlocal
exit /b 0