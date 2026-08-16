"""
conftest.py - Global pytest fixtures for PXView MCP E2E tests.

Provides:
- mcp: A connected McpClient instance (session-scoped)
- demo_device: The demo device info dict
- device_id: Device ID string of the demo device
- tmp_capture_dir: Temporary directory for capture/export files
- cleanup_after_test: Fixture that ensures clean state between tests

This conftest uses pxview-automation's PXViewProcess to automatically
start/stop PXView in headless mode, eliminating the need for external
startup scripts.  If PXView is already running, it connects to the
existing instance instead.

Configuration via environment variables:
    PXVIEW_MCP_URL          MCP endpoint URL (default: http://127.0.0.1:10110/mcp)
    PXVIEW_MCP_PORT         MCP port (default: 10110, used when auto-starting)
    PXVIEW_WS_PORT          WebSocket port (default: 10430, used when auto-starting)
    PXVIEW_STARTUP_TIMEOUT  Seconds to wait for server (default: 120)
    PXVIEW_EXE_PATH         Path to PXView.exe; if set, auto-start headless
    PXVIEW_NO_AUTO_START    If "1", never auto-start (assume server is running)
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import time
from pathlib import Path
from typing import Iterator, Optional

import pytest

# Ensure the pxview-automation package is importable.  The package may be
# pip-installed (preferred) or used directly from source.
try:
    from pxview_automation import McpClient, McpError, PXViewProcess
except ImportError:
    # Fall back to source tree
    _pkg_src = Path(__file__).resolve().parent.parent / "pxview-automation" / "src"
    if str(_pkg_src) not in sys.path:
        sys.path.insert(0, str(_pkg_src))
    from pxview_automation import McpClient, McpError, PXViewProcess


# ---- Configuration (overridable via environment variables) ----

MCP_URL = os.environ.get("PXVIEW_MCP_URL", "http://127.0.0.1:10110/mcp")
MCP_PORT = int(os.environ.get("PXVIEW_MCP_PORT", "10110"))
WS_PORT = int(os.environ.get("PXVIEW_WS_PORT", "10430"))
MCP_STARTUP_TIMEOUT = float(os.environ.get("PXVIEW_STARTUP_TIMEOUT", "120"))
EXE_PATH = os.environ.get("PXVIEW_EXE_PATH", "")
NO_AUTO_START = os.environ.get("PXVIEW_NO_AUTO_START", "") == "1"


# ---- Session-scoped fixtures ----

@pytest.fixture(scope="session")
def _pxview_process() -> Iterator[Optional[PXViewProcess]]:
    """Start PXView headless if needed, or return None if already running.

    This fixture is session-scoped so PXView is started once for the
    entire test run and stopped when all tests are done.

    If PXVIEW_NO_AUTO_START=1 is set, or if the MCP server is already
    reachable, no process is started.
    """
    proc: Optional[PXViewProcess] = None

    # Check if MCP server is already reachable
    probe = McpClient(url=MCP_URL, timeout=5.0, max_retries=1)
    if probe.wait_for_server(timeout=3.0, interval=0.5):
        # Server already running — no need to start PXView
        yield None
        return

    if NO_AUTO_START:
        pytest.fail(
            f"PXVIEW_NO_AUTO_START=1 but MCP server at {MCP_URL} is not "
            f"reachable. Start PXView --headless first."
        )

    # Determine the exe path
    exe = EXE_PATH or None

    # If no exe path given, try common build/package locations relative to repo
    if not exe:
        repo_root = Path(__file__).resolve().parent.parent
        # Platform-aware: on Windows only *.exe is runnable (a stray
        # extension-less `build/PXView` Linux binary must not shadow it);
        # on POSIX prefer the extension-less binary.
        win = sys.platform.startswith("win")
        candidates = []
        if win:
            candidates = [
                repo_root / "build" / "PXView.exe",        # Windows build dir
                repo_root / "build.dir" / "PXView.exe",    # Windows MSYS2 build out
                repo_root / "package" / "PXView.exe",      # Windows package dir
                repo_root / "install.dir" / "bin" / "PXView.exe",  # Windows install
                repo_root / "PXView" / "build" / "PXView.exe",
            ]
        else:
            candidates = [
                repo_root / "build" / "PXView",            # Linux build dir
                repo_root / "package" / "PXView",          # Linux package dir
                repo_root / "install.dir" / "usr" / "bin" / "PXView",  # Linux install
                repo_root / "PXView" / "build" / "PXView",
            ]
        for c in candidates:
            if c.is_file():
                exe = str(c)
                break

    if not exe:
        # Let PXViewProcess search PATH and common install dirs
        exe = None

    try:
        proc = PXViewProcess(
            exe_path=exe,
            port=MCP_PORT,
            ws_port=WS_PORT,
            startup_timeout=MCP_STARTUP_TIMEOUT,
        )
        proc.start()
    except Exception as exc:
        pytest.fail(
            f"Failed to start PXView headless: {exc}\n"
            f"Set PXVIEW_EXE_PATH to point to PXView.exe, or start it "
            f"manually with: PXView.exe --headless -l 4"
        )

    yield proc

    # Cleanup: stop PXView
    if proc is not None:
        proc.stop()


@pytest.fixture(scope="session")
def mcp(_pxview_process) -> Iterator[McpClient]:
    """Provide a connected MCP client for the entire test session."""
    client = McpClient(
        url=MCP_URL, timeout=120.0, max_retries=5, retry_delay=1.0
    )
    # Wait for server to be reachable (covers both auto-started and external)
    if not client.wait_for_server(timeout=MCP_STARTUP_TIMEOUT, interval=1.0):
        pytest.fail(
            f"Cannot connect to MCP server at {MCP_URL} within "
            f"{MCP_STARTUP_TIMEOUT}s. Is PXView running?"
        )
    # Perform MCP handshake
    client.connect()
    assert client.connected, "MCP connect() returned but connected=False"
    # Verify we got tools
    assert len(client.tools) > 0, "tools/list returned 0 tools"
    yield client
    # Cleanup: close any capture, clear decoders
    try:
        client.clear_all_decoders()
    except Exception:
        pass
    try:
        client.close_capture()
    except Exception:
        pass
    client.disconnect()


@pytest.fixture(scope="session")
def demo_device(mcp: McpClient) -> dict:
    """Return the demo device info dict."""
    return mcp.get_demo_device()


@pytest.fixture(scope="session")
def device_id(demo_device: dict) -> str:
    """Return the device ID string of the demo device."""
    return demo_device["id"]


@pytest.fixture(autouse=False)
def ensure_device_connected(mcp: McpClient, device_id: str):
    """Ensure the demo device is connected before the test.
    Use this fixture when a test requires an active device session."""
    try:
        mcp.connect_device(device_id)
    except Exception:
        pass  # May already be connected
    yield


# ---- Function-scoped fixtures ----

@pytest.fixture
def tmp_capture_dir() -> Iterator[str]:
    """Provide a temporary directory for capture/export files."""
    d = tempfile.mkdtemp(prefix="pxview_test_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture
def tmp_pxc_file(tmp_capture_dir: str) -> str:
    """Provide a temporary .pxc file path."""
    return os.path.join(tmp_capture_dir, "test_capture.pxc")


@pytest.fixture
def tmp_pxl_file(tmp_capture_dir: str) -> str:
    """Provide a temporary .pxl file path."""
    return os.path.join(tmp_capture_dir, "test_capture.pxl")


@pytest.fixture(autouse=True)
def cleanup_after_test(mcp: McpClient):
    """Ensure clean state before and after each test."""
    # Pre-test: ensure server is responsive
    mcp.wait_for_server(timeout=30, interval=2.0)
    # Pre-test cleanup — use short timeouts and fewer retries
    orig_retries = mcp.max_retries
    mcp.max_retries = 2
    for cleanup_fn in [
        lambda: mcp._call_tool("stop_capture", {}, timeout=5.0),
        lambda: mcp._call_tool("clear_all_decoders", {}, timeout=5.0),
        lambda: mcp._call_tool("close_capture", {}, timeout=5.0),
    ]:
        try:
            cleanup_fn()
        except Exception:
            pass
    yield
    # Post-test cleanup — same short timeouts
    for cleanup_fn in [
        lambda: mcp._call_tool("stop_capture", {}, timeout=5.0),
        lambda: mcp._call_tool("clear_all_decoders", {}, timeout=5.0),
        lambda: mcp._call_tool("clear_cursors", {}, timeout=5.0),
        lambda: mcp._call_tool("close_capture", {}, timeout=5.0),
    ]:
        try:
            cleanup_fn()
        except Exception:
            pass
    mcp.max_retries = orig_retries
    # Post-test: ensure server is responsive for next test
    mcp.wait_for_server(timeout=30, interval=2.0)


@pytest.fixture
def capture_10s(mcp: McpClient, device_id: str, cleanup_after_test):
    """Perform a 10-second demo capture and yield when complete.

    Returns the capture status dict after completion.
    """
    logic_config = {
        "digitalChannels": [0, 1, 2, 3],
        "digitalSampleRate": 1000000,
    }
    capture_config = {
        "timedCaptureMode": {"durationSeconds": 10}
    }
    mcp.start_capture(device_id, logic_config, capture_config)
    mcp.wait_capture(timeout_seconds=30, timeout=40)
    # Small delay for post-capture processing
    time.sleep(1)
    status = mcp.get_capture_status()
    yield status
