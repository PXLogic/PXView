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


def do_buffer_capture_with_pattern(
    mcp: McpClient,
    device_id: str,
    channels: list,
    sample_rate: int,
    sample_count: int,
    pattern: str,
) -> dict:
    """Buffer-mode capture with a specific demo pattern.

    Args:
        pattern: 'random', 'graycode', 'i2c', 'sigrok', 'incremental', etc.
    """
    logic_config = {
        "digitalChannels": channels,
        "digitalSampleRate": sample_rate,
        "pattern": pattern,
    }
    capture_config = {
        "manualCaptureMode": {"sampleCount": sample_count},
    }
    mcp.start_capture(
        device_id=device_id,
        logic_device_configuration=logic_config,
        capture_configuration=capture_config,
    )
    wait_timeout = max(sample_count / sample_rate * 3, 30)
    mcp.wait_capture(timeout_seconds=wait_timeout, timeout=wait_timeout + 10)
    time.sleep(0.5)
    return mcp.get_capture_status()


def configure_pwm(mcp: McpClient, channel: int, enable: bool,
                  freq: float, duty: float):
    """Configure demo PWM output on ch6 (pwm0) or ch7 (pwm1).

    Uses set_config with SR_CONF_PWM0/1_* keys:
      PWM0_EN=60004, PWM0_FREQ=60005, PWM0_DUTY=60006
      PWM1_EN=60007, PWM1_FREQ=60008, PWM1_DUTY=60009
    """
    if channel == 0:
        en_key, freq_key, duty_key = 60004, 60005, 60006
    elif channel == 1:
        en_key, freq_key, duty_key = 60007, 60008, 60009
    else:
        raise ValueError(f"PWM channel must be 0 or 1, got {channel}")
    mcp.set_config(key=en_key, value_type="bool", value=enable)
    if enable:
        mcp.set_config(key=freq_key, value_type="double", value=freq)
        mcp.set_config(key=duty_key, value_type="double", value=duty)

