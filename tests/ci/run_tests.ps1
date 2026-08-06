# ci/run_tests.ps1 - Windows 本地 E2E 测试脚本
#
# 用法：
#   1. 先在本地编译并打包 portable 包（执行 window/package.sh 后生成的 package/ 目录）
#   2. 运行此脚本：
#      .\tests\ci\run_tests.ps1 -PackageDir .\package\
#
# 参数：
#   -PackageDir    portable 包目录（包含 PXView.exe 的目录），必填
#   -McpUrl        MCP 服务地址，默认 http://127.0.0.1:10110/mcp
#   -StartupTimeout  等待 MCP 启动的超时秒数，默认 90
#   -PytestArgs    额外的 pytest 参数，默认为空

param(
    [Parameter(Mandatory=$true)]
    [string]$PackageDir,

    [string]$McpUrl = "http://127.0.0.1:10110/mcp",
    [int]$StartupTimeout = 90,
    [string]$PytestArgs = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path $MyInvocation.MyCommand.Path -Parent
$ProjectRoot = Resolve-Path "$ScriptDir\..\.."
$TestsDir = Join-Path $ProjectRoot "tests"

# ============================================================
# 1. 验证 portable 包
# ============================================================
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  PXView E2E Test Runner (Local)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Package dir:  $PackageDir"
Write-Host "Tests dir:    $TestsDir"
Write-Host "MCP URL:      $McpUrl"
Write-Host ""

$pkgDir = (Resolve-Path $PackageDir).Path
$pxvExe = Join-Path $pkgDir "PXView.exe"

if (-not (Test-Path $pxvExe)) {
    Write-Host "ERROR: PXView.exe not found in $pkgDir" -ForegroundColor Red
    Write-Host "Make sure you have built and packaged PXView first."
    Write-Host "  1. cmake --build build  (or ninja -C build)"
    Write-Host "  2. bash window/package.sh"
    Write-Host "  3. .\tests\ci\run_tests.ps1 -PackageDir .\package\"
    exit 1
}

Write-Host "PXView.exe:   $pxvExe ($((Get-Item $pxvExe).Length) bytes)"

$dlls = Get-ChildItem "$pkgDir\*.dll" -ErrorAction SilentlyContinue
Write-Host "DLLs found:   $($dlls.Count)"

if (Test-Path "$pkgDir\c_decoders") {
    $cDecs = Get-ChildItem "$pkgDir\c_decoders" -ErrorAction SilentlyContinue
    Write-Host "C decoders:   $($cDecs.Count) entries"
}

Write-Host ""

# ============================================================
# 2. 查找有 pytest 的 Python 解释器
# ============================================================
Write-Host "=== Finding Python with pytest ===" -ForegroundColor Yellow

$pythonCandidates = @(
    "python",
    "python3",
    "C:\msys64\usr\bin\python3.exe",
    "C:\msys64\mingw64\bin\python.exe"
)

$pythonExe = $null
foreach ($candidate in $pythonCandidates) {
    try {
        $ver = & $candidate --version 2>$null
        if ($ver) {
            $hasPytest = & $candidate -c "import pytest; print('OK')" 2>$null
            $hasRequests = & $candidate -c "import requests; print('OK')" 2>$null
            if ($hasPytest -eq "OK" -and $hasRequests -eq "OK") {
                $pythonExe = $candidate
                Write-Host "Found: $candidate ($ver) with pytest + requests"
                break
            } else {
                Write-Host "  $candidate ($ver) - missing modules (pytest=$hasPytest, requests=$hasRequests)"
            }
        }
    } catch { }
}

if (-not $pythonExe) {
    # Try to install into the default python
    Write-Host "No Python with pytest found, trying to install..."
    $pythonExe = "python"
    try {
        & $pythonExe -m pip install --quiet pytest pytest-html pytest-timeout requests 2>$null
        $check = & $pythonExe -c "import pytest; print('OK')" 2>$null
        if ($check -ne "OK") { throw "pip install failed" }
        Write-Host "Installed pytest into $pythonExe"
    } catch {
        # Try MSYS2 pacman approach
        Write-Host "Trying MSYS2 pacman..."
        C:\msys64\usr\bin\bash.exe -lc 'pacman -S --noconfirm python-pip 2>/dev/null; pip3 install --break-system-packages pytest pytest-html requests 2>&1' 2>$null
        $pythonExe = "C:\msys64\usr\bin\python3.exe"
        $check = & $pythonExe -c "import pytest; print('OK')" 2>$null
        if ($check -ne "OK") {
            Write-Host "ERROR: Cannot find or install pytest. Please install manually:" -ForegroundColor Red
            Write-Host "  pip install pytest pytest-html requests"
            exit 1
        }
        Write-Host "Installed pytest via MSYS2 pacman"
    }
}

Write-Host "Using Python: $pythonExe"
Write-Host ""

# ============================================================
# 3. 清理残留进程和端口
# ============================================================
Write-Host "=== Cleaning up leftover PXView processes ===" -ForegroundColor Yellow

# 杀掉所有残留的 PXView 进程
$leftover = Get-Process -Name "PXView" -ErrorAction SilentlyContinue
if ($leftover) {
    Write-Host "Found $($leftover.Count) leftover PXView process(es), killing..."
    $leftover | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
} else {
    Write-Host "No leftover PXView processes."
}

# 等待端口 10110 释放（TimeWait 状态的连接需要时间清除）
Write-Host "Checking port 10110..."
$portWait = 0
while ($portWait -lt 30) {
    $conns = Get-NetTCPConnection -LocalPort 10110 -State Listen -ErrorAction SilentlyContinue
    if (-not $conns) {
        Write-Host "Port 10110 is free."
        break
    }
    $portWait++
    Start-Sleep -Seconds 1
}
if ($portWait -ge 30) {
    Write-Host "WARNING: Port 10110 still has connections after 30s, proceeding anyway..." -ForegroundColor DarkYellow
}

# ============================================================
# 4. 启动 PXView headless
# ============================================================
Write-Host ""
Write-Host "=== Starting PXView in headless mode ===" -ForegroundColor Yellow

$logFile = Join-Path $env:TEMP "pxview_headless.log"
$errFile = Join-Path $env:TEMP "pxview_headless.err"

# 如果有旧日志，先删除
Remove-Item $logFile, $errFile -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $pxvExe `
    -ArgumentList "--headless", "-l", "4" `
    -WorkingDirectory $pkgDir `
    -RedirectStandardOutput $logFile `
    -RedirectStandardError $errFile `
    -PassThru -NoNewWindow

Write-Host "PXView PID: $($proc.Id)"
$proc.Id | Out-File -FilePath (Join-Path $env:TEMP "pxview_pid.txt") -NoNewline

# ============================================================
# 5. 等待 MCP 服务就绪
# ============================================================
Write-Host ""
Write-Host "=== Waiting for MCP server to be ready ===" -ForegroundColor Yellow
Write-Host "(PXView loads 200+ Python decoders on startup, this takes ~50-60s)"
$ready = $false
$lastLogLine = 0

for ($i = 1; $i -le $StartupTimeout; $i++) {
    if ($proc.HasExited) {
        Write-Host ""
        Write-Host "ERROR: PXView exited prematurely (exit code: $($proc.ExitCode))" -ForegroundColor Red
        Write-Host ""
        Write-Host "=== stdout log ===" -ForegroundColor Yellow
        if (Test-Path $logFile) { Get-Content $logFile -Tail 60 }
        Write-Host ""
        Write-Host "=== stderr log ===" -ForegroundColor Yellow
        if (Test-Path $errFile) { Get-Content $errFile -Tail 60 }
        exit 1
    }

    # Show PXView startup progress from log
    if (Test-Path $errFile) {
        $lines = Get-Content $errFile -ErrorAction SilentlyContinue
        if ($lines) {
            $newLines = $lines | Select-Object -Skip $lastLogLine
            foreach ($line in $newLines) {
                if ($line -match "DBG:|Headless mode started|init|decoder_load") {
                    Write-Host "  [PXView] $line" -ForegroundColor DarkGray
                }
            }
            $lastLogLine = $lines.Count
        }
    }
    if (Test-Path $logFile) {
        $lines = Get-Content $logFile -ErrorAction SilentlyContinue
        if ($lines) {
            $newLines = $lines | Select-Object -Skip $lastLogLine
            foreach ($line in $newLines) {
                Write-Host "  [PXView] $line" -ForegroundColor DarkGray
            }
            $lastLogLine = $lines.Count
        }
    }

    # Use curl.exe instead of Invoke-RestMethod to avoid PowerShell proxy issues
    $body = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"ci","version":"1.0"}}}'
    $resp = curl.exe -s -X POST $McpUrl -H "Content-Type: application/json" -d $body --max-time 5 2>$null
    if ($resp -and $resp -match "protocolVersion") {
        Write-Host "MCP server ready after ${i}s" -ForegroundColor Green
        $ready = $true
        break
    }

    if ($i % 10 -eq 0 -and $i -lt $StartupTimeout) {
        Write-Host "  Still waiting... (${i}s / ${StartupTimeout}s)" -ForegroundColor DarkYellow
    }
    Start-Sleep -Seconds 1
}

if (-not $ready) {
    Write-Host ""
    Write-Host "ERROR: MCP server did not start within ${StartupTimeout}s" -ForegroundColor Red
    Write-Host ""
    Write-Host "=== stdout log (last 80 lines) ===" -ForegroundColor Yellow
    if (Test-Path $logFile) { Get-Content $logFile -Tail 80 }
    Write-Host ""
    Write-Host "=== stderr log (last 80 lines) ===" -ForegroundColor Yellow
    if (Test-Path $errFile) { Get-Content $errFile -Tail 80 }
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

# ============================================================
# 6. 运行 pytest E2E 测试
# ============================================================
Write-Host ""
Write-Host "=== Running pytest E2E test suite ===" -ForegroundColor Yellow

Push-Location $TestsDir
$env:PXVIEW_MCP_URL = $McpUrl
$env:PXVIEW_STARTUP_TIMEOUT = "30"

$reportDir = $ProjectRoot.Replace('\', '/')
$htmlReport = Join-Path $reportDir "test_report.html"
$xmlReport = Join-Path $reportDir "test_report.xml"
# Use forward slashes for Cygwin/MSYS2 Python compatibility
$htmlReport = $htmlReport.Replace('\', '/')
$xmlReport = $xmlReport.Replace('\', '/')

$pytestCmd = "$pythonExe -m pytest suites/ -v --tb=short --html=`"$htmlReport`" --junitxml=`"$xmlReport`" --self-contained-html"
if ($PytestArgs) {
    $pytestCmd += " $PytestArgs"
}

Write-Host "Command: $pytestCmd"
Write-Host ""

$testExit = 0
try {
    Invoke-Expression $pytestCmd
    $testExit = $LASTEXITCODE
} catch {
    $testExit = 1
}

Pop-Location

# ============================================================
# 7. 停止 PXView
# ============================================================
Write-Host ""
Write-Host "=== Stopping PXView ===" -ForegroundColor Yellow
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Host ""
Write-Host "=== PXView headless log (last 30 lines) ===" -ForegroundColor DarkGray
if (Test-Path $logFile) {
    Get-Content $logFile -Tail 30
}

# ============================================================
# 8. 汇总结果
# ============================================================
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Test Results" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

if ($testExit -eq 0) {
    Write-Host "  ALL TESTS PASSED" -ForegroundColor Green
} else {
    Write-Host "  SOME TESTS FAILED (exit code: $testExit)" -ForegroundColor Red
}

Write-Host ""
Write-Host "  HTML report: $htmlReport"
Write-Host "  JUnit XML:   $xmlReport"
Write-Host ""

exit $testExit
