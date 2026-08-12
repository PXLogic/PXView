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

    Uses ``set_config`` to write SR_CONF_* keys for PWM enable,
    frequency, and duty cycle.

    Args:
        mcp:     MCP client instance.
        channel: 6 for pwm0, 7 for pwm1.
        enable:  True to enable PWM output.
        freq:    PWM frequency in Hz.
        duty:    Duty cycle (0.0 – 1.0).
    """
    pwm_idx = channel - 6  # 0 or 1
    if pwm_idx not in (0, 1):
        raise ValueError(f"PWM channel must be 6 or 7, got {channel}")

    # SR_CONF keys for PWM (these match the libsigrok demo driver)
    SR_CONF_PWM_ENABLE = 0x40001 + pwm_idx
    SR_CONF_PWM_FREQ   = 0x40003 + pwm_idx
    SR_CONF_PWM_DUTY   = 0x40005 + pwm_idx

    mcp.set_config(key=SR_CONF_PWM_ENABLE, type="bool", value=enable)
    mcp.set_config(key=SR_CONF_PWM_FREQ,   type="double", value=freq)
    mcp.set_config(key=SR_CONF_PWM_DUTY,   type="double", value=duty)

