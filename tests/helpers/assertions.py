"""
assertions.py - Custom assertion helpers for PXView MCP E2E tests.
"""

from __future__ import annotations

from typing import Any, Dict, List

from mcp_client import McpClient


def assert_device_valid(device: dict) -> None:
    """Assert that a device dict has all required fields."""
    required = ["id", "display_name", "is_demo"]
    for field in required:
        assert field in device, f"Device missing required field '{field}': {device}"


def assert_channel_valid(channel: dict) -> None:
    """Assert that a channel dict has all required fields."""
    required = ["index", "name", "type", "enabled"]
    for field in required:
        assert field in channel, f"Channel missing required field '{field}': {channel}"


def assert_capture_status(status: dict, expected_state: str = None) -> None:
    """Assert capture status is valid and optionally matches expected state."""
    assert "state" in status, f"Capture status missing 'state': {status}"
    valid_states = ["idle", "capturing", "completed", "stopped"]
    assert status["state"] in valid_states, \
        f"Invalid capture state '{status['state']}'. Expected one of {valid_states}"
    if expected_state:
        assert status["state"] == expected_state, \
            f"Capture state '{status['state']}' != expected '{expected_state}'"


def assert_annotation_valid(ann: dict) -> None:
    """Assert that a decoder annotation has the expected structure."""
    assert "start_sample" in ann, f"Annotation missing 'start_sample': {ann}"
    assert "end_sample" in ann, f"Annotation missing 'end_sample': {ann}"
    assert "ann_class" in ann, f"Annotation missing 'ann_class': {ann}"
    assert "texts" in ann, f"Annotation missing 'texts': {ann}"
    assert isinstance(ann["texts"], list), \
        f"Annotation 'texts' should be a list: {ann}"
    assert ann["start_sample"] >= 0, \
        f"Annotation start_sample < 0: {ann}"
    assert ann["end_sample"] >= ann["start_sample"], \
        f"Annotation end_sample < start_sample: {ann}"


def assert_analyzer_list_valid(analyzers: List[dict]) -> None:
    """Assert that the analyzer list is a valid list of decoder definitions."""
    assert isinstance(analyzers, list), "Analyzers should be a list"
    assert len(analyzers) > 0, "Analyzer list should not be empty"
    for a in analyzers:
        assert "id" in a or "name" in a, \
            f"Analyzer missing id/name: {a}"


def assert_analyzer_options_valid(options: dict) -> None:
    """Assert that analyzer options have the expected structure."""
    assert isinstance(options, dict), "Options should be a dict"
    # Should have channels or optional_channels
    has_channels = "channels" in options or "required_channels" in options
    has_optional = "optional_channels" in options
    assert has_channels or has_optional, \
        f"Options should have channels or optional_channels: {options}"


def assert_no_error(mcp: McpClient) -> None:
    """Assert the session has no error state."""
    err = mcp.get_error_state()
    assert not err.get("has_error", False), \
        f"Session has unexpected error: {err.get('error_message', '')}"


def assert_samples_non_empty(samples: Any) -> None:
    """Assert that samples data is non-empty."""
    if isinstance(samples, (bytes, bytearray)):
        assert len(samples) > 0, "Samples bytes should not be empty"
    elif isinstance(samples, list):
        assert len(samples) > 0, "Samples list should not be empty"
    else:
        assert samples is not None, "Samples should not be None"
