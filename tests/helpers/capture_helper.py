"""
capture_helper.py - Helper functions for capture operations.
"""

from __future__ import annotations

import time
from typing import Optional

from pxview_automation import McpClient


def do_timed_capture(mcp: McpClient, device_id: str,
                     channels: list = None,
                     sample_rate: int = 1000000,
                     duration_seconds: float = 1.0,
                     analog_channels: list = None) -> dict:
    """Perform a timed capture and return the status.

    Handles start_capture -> wait_capture -> get_capture_status.
    """
    if channels is None:
        channels = [0, 1]
    logic_config: dict = {
        "digitalChannels": channels,
        "digitalSampleRate": sample_rate,
    }
    if analog_channels:
        logic_config["analogChannels"] = analog_channels
        logic_config["analogSampleRate"] = sample_rate
    capture_config = {
        "timedCaptureMode": {"durationSeconds": duration_seconds}
    }
    mcp.start_capture(device_id, logic_config, capture_config)
    wait_timeout = max(duration_seconds * 3, 30)
    mcp.wait_capture(timeout_seconds=wait_timeout, timeout=wait_timeout + 10)
    time.sleep(0.5)
    return mcp.get_capture_status()


def do_manual_capture(mcp: McpClient, device_id: str,
                      channels: list = None,
                      sample_rate: int = 1000000,
                      sample_count: int = 100000) -> dict:
    """Perform a manual (sample-count) capture and return the status."""
    if channels is None:
        channels = [0, 1]
    logic_config = {
        "digitalChannels": channels,
        "digitalSampleRate": sample_rate,
    }
    capture_config = {
        "manualCaptureMode": {"sampleCount": sample_count}
    }
    mcp.start_capture(device_id, logic_config, capture_config)
    mcp.wait_capture(timeout_seconds=60, timeout=70)
    time.sleep(0.5)
    return mcp.get_capture_status()


def wait_for_decode(mcp: McpClient, analyzer_id: str,
                    max_wait: float = 30.0,
                    poll_interval: float = 1.0) -> list:
    """Wait for decoder to produce results, polling periodically."""
    deadline = time.time() + max_wait
    while time.time() < deadline:
        try:
            results = mcp.get_analyzer_results(analyzer_id, max_count=1)
            if results and len(results) > 0:
                return mcp.get_analyzer_results(analyzer_id, max_count=10000)
        except Exception:
            pass
        time.sleep(poll_interval)
    # Final attempt
    return mcp.get_analyzer_results(analyzer_id, max_count=10000)
