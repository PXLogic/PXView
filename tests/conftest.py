"""
conftest.py - Global pytest fixtures for PXView MCP E2E tests.

Provides:
- mcp: A connected McpClient instance (session-scoped)
- demo_device: The demo device info dict
- device_id: Device ID string of the demo device
- tmp_capture_dir: Temporary directory for capture/export files
- cleanup_after_test: Fixture that ensures clean state between tests
"""

from __future__ import annotations

import os
import shutil
import tempfile
import time
from pathlib import Path
from typing import Iterator

import pytest

from mcp_client import McpClient, McpError, McpConnectionError


# ---- Configuration (overridable via environment variables) ----

MCP_URL = os.environ.get("PXVIEW_MCP_URL", "http://127.0.0.1:10110/mcp")
MCP_STARTUP_TIMEOUT = float(os.environ.get("PXVIEW_STARTUP_TIMEOUT", "60"))


# ---- Session-scoped fixtures ----

@pytest.fixture(scope="session")
def mcp() -> Iterator[McpClient]:
    """Provide a connected MCP client for the entire test session."""
    client = McpClient(url=MCP_URL, timeout=120.0, max_retries=5,
                       retry_delay=1.0)
    # Wait for server to be reachable
    if not client.wait_for_server(timeout=MCP_STARTUP_TIMEOUT, interval=1.0):
        pytest.fail(
            f"Cannot connect to MCP server at {MCP_URL} within "
            f"{MCP_STARTUP_TIMEOUT}s. Is PXView running?"
        )
    # Perform MCP handshake
    client.connect()
    assert client.connected, "MCP connect() returned but connected=False"
    # Verify we got tools
    assert len(client.tools) > 0, f"tools/list returned 0 tools"
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
