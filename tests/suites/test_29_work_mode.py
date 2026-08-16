"""
test_29_work_mode.py - Work mode management tests.

Covers 3 previously uncovered MCP tools:
  - switch_work_mode
  - get_work_mode
  - get_supported_work_modes

Work modes: 0=Logic, 1=DSO, 2=Analog, 3=MSO.
All tests start in Logic mode (default) and verify mode switching
works correctly, then restore Logic mode in cleanup.
"""

import pytest

from pxview_automation import McpClient

pytestmark = pytest.mark.p1


class TestWorkMode:
    """Test work mode management tools."""

    def test_get_supported_work_modes(self, mcp: McpClient, device_id: str,
                                       ensure_device_connected,
                                       cleanup_after_test):
        """get_supported_work_modes returns a list including Logic (0).

        get_supported_work_modes reads the connected device's mode list, so
        the demo device must be connected first (unlike get_work_mode /
        switch_work_mode which operate on session state alone).
        """
        modes = mcp.get_supported_work_modes()
        assert isinstance(modes, list), f"Expected list, got {type(modes)}"
        assert len(modes) > 0, "No supported work modes returned"
        assert 0 in modes, \
            f"Logic mode (0) should always be supported, got: {modes}"

    def test_get_work_mode_default(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """Default work mode is Logic (0)."""
        mode = mcp.get_work_mode()
        assert mode == 0, \
            f"Default work mode should be Logic (0), got {mode}"

    def test_switch_to_logic_mode(self, mcp: McpClient, device_id: str,
                                   cleanup_after_test):
        """switch_work_mode to Logic (0) succeeds and is reflected by get_work_mode."""
        result = mcp.switch_work_mode(mode=0)
        assert result is not None
        mode = mcp.get_work_mode()
        assert mode == 0, f"After switch to Logic, get_work_mode returned {mode}"

    def test_switch_and_verify_all_modes(self, mcp: McpClient, device_id: str,
                                          ensure_device_connected,
                                          cleanup_after_test):
        """Switch to each supported mode and verify get_work_mode reflects it.

        After testing, restore to Logic mode (0).
        """
        modes = mcp.get_supported_work_modes()
        assert 0 in modes

        for mode in modes:
            mcp.switch_work_mode(mode=mode)
            actual = mcp.get_work_mode()
            assert actual == mode, \
                f"Switched to mode {mode} but get_work_mode returned {actual}"

        # Restore Logic mode
        mcp.switch_work_mode(mode=0)
        assert mcp.get_work_mode() == 0

    def test_switch_work_mode_idempotent(self, mcp: McpClient, device_id: str,
                                          cleanup_after_test):
        """Switching to the same mode twice is safe."""
        mcp.switch_work_mode(mode=0)
        mcp.switch_work_mode(mode=0)
        assert mcp.get_work_mode() == 0

    def test_get_supported_work_modes_consistent(self, mcp: McpClient,
                                                   device_id: str,
                                                   ensure_device_connected,
                                                   cleanup_after_test):
        """get_supported_work_modes returns consistent results across calls."""
        modes1 = mcp.get_supported_work_modes()
        modes2 = mcp.get_supported_work_modes()
        assert modes1 == modes2, \
            f"Inconsistent modes: {modes1} vs {modes2}"
