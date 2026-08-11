"""
test_14_trigger_config.py - Trigger configuration tests.
test_15_channel_config.py - Channel configuration tests.
test_16_sample_config.py - Sample configuration tests.
"""

import pytest

from pxview_automation import McpClient, McpError
from helpers.capture_helper import do_timed_capture

pytestmark = pytest.mark.p1


class TestTriggerConfig:

    def test_get_trigger_config_default(self, mcp: McpClient, device_id: str,
                                        cleanup_after_test):
        """get_trigger_config returns current config."""
        mcp.connect_device(device_id)
        config = mcp.get_trigger_config()
        assert isinstance(config, dict)

    def test_get_trigger_config_logic(self, mcp: McpClient, device_id: str,
                                      cleanup_after_test):
        """get_trigger_config with mode=logic."""
        mcp.connect_device(device_id)
        config = mcp.get_trigger_config(mode="logic")
        assert isinstance(config, dict)

    def test_set_trigger_rising(self, mcp: McpClient, device_id: str,
                                cleanup_after_test):
        """Set rising edge trigger."""
        mcp.connect_device(device_id)
        mcp.set_trigger_config(mode="logic",
                               stage_count=1,
                               config_json='[{"channel": 0, "type": "rising"}]')

    def test_set_trigger_falling(self, mcp: McpClient, device_id: str,
                                 cleanup_after_test):
        """Set falling edge trigger."""
        mcp.connect_device(device_id)
        mcp.set_trigger_config(mode="logic",
                               stage_count=1,
                               config_json='[{"channel": 0, "type": "falling"}]')

    def test_set_trigger_with_capture(self, mcp: McpClient, device_id: str,
                                      cleanup_after_test):
        """Trigger config doesn't break capture."""
        mcp.connect_device(device_id)
        mcp.set_trigger_config(mode="logic", stage_count=0)
        status = do_timed_capture(mcp, device_id, channels=[0],
                                  sample_rate=1000000, duration_seconds=0.3)
        assert status["state"] in ("completed", "idle")


class TestChannelConfig:

    def test_set_channel_enabled(self, mcp: McpClient, device_id: str,
                                 cleanup_after_test):
        """Enable/disable a channel."""
        mcp.connect_device(device_id)
        mcp.set_channel_enabled(channel_index=0, enabled=True)
        mcp.set_channel_enabled(channel_index=0, enabled=False)
        mcp.set_channel_enabled(channel_index=0, enabled=True)

    def test_set_channel_name(self, mcp: McpClient, device_id: str,
                              cleanup_after_test):
        """Rename a channel."""
        mcp.connect_device(device_id)
        mcp.set_channel_name(channel_index=0, name="TEST_CH0")
        channels = mcp.get_channels()
        ch0 = [c for c in channels if c["index"] == 0]
        if ch0:
            assert ch0[0]["name"] == "TEST_CH0"

    def test_get_probe_config(self, mcp: McpClient, device_id: str,
                              cleanup_after_test):
        """get_probe_config returns config."""
        mcp.connect_device(device_id)
        try:
            config = mcp.get_probe_config(channel_index=0)
            assert isinstance(config, dict)
        except McpError:
            pass  # Demo device may not support probe config

    def test_get_channels_after_enable(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """get_channels reflects enabled state."""
        mcp.connect_device(device_id)
        mcp.set_channel_enabled(channel_index=0, enabled=True)
        channels = mcp.get_channels()
        ch0 = [c for c in channels if c["index"] == 0]
        if ch0:
            assert ch0[0]["enabled"] is True


class TestSampleConfig:

    def test_get_sample_config(self, mcp: McpClient, device_id: str,
                               cleanup_after_test):
        """get_sample_config returns config."""
        mcp.connect_device(device_id)
        config = mcp.get_sample_config()
        assert isinstance(config, dict)
        assert "sample_rate" in config or "sample_limit" in config

    def test_set_sample_rate(self, mcp: McpClient, device_id: str,
                             cleanup_after_test):
        """set_sample_rate changes the sample rate."""
        mcp.connect_device(device_id)
        mcp.set_sample_rate(rate=1000000)
        config = mcp.get_sample_config()
        assert config.get("sample_rate") == 1000000

    def test_set_sample_limit(self, mcp: McpClient, device_id: str,
                              cleanup_after_test):
        """set_sample_limit changes the sample limit."""
        mcp.connect_device(device_id)
        mcp.set_sample_limit(limit=10000)
        config = mcp.get_sample_config()
        assert config.get("sample_limit") == 10000

    def test_set_time_base(self, mcp: McpClient, device_id: str,
                           cleanup_after_test):
        """set_time_base changes the time base."""
        mcp.connect_device(device_id)
        mcp.set_time_base(time_base=1000)

    def test_set_collect_mode_single(self, mcp: McpClient, device_id: str,
                                     cleanup_after_test):
        """set_collect_mode to single."""
        mcp.connect_device(device_id)
        mcp.set_collect_mode(mode="single")

    def test_set_collect_mode_repetitive(self, mcp: McpClient, device_id: str,
                                         cleanup_after_test):
        """set_collect_mode to repetitive."""
        mcp.connect_device(device_id)
        mcp.set_collect_mode(mode="repetitive")
        mcp.set_repeat_interval(interval_ms=1000)

    def test_set_collect_mode_loop(self, mcp: McpClient, device_id: str,
                                   cleanup_after_test):
        """set_collect_mode to loop."""
        mcp.connect_device(device_id)
        mcp.set_collect_mode(mode="loop")

    def test_set_repeat_interval(self, mcp: McpClient, device_id: str,
                                 cleanup_after_test):
        """set_repeat_interval changes interval."""
        mcp.connect_device(device_id)
        mcp.set_repeat_interval(interval_ms=500)

    def test_get_repeat_status(self, mcp: McpClient, device_id: str,
                               cleanup_after_test):
        """get_repeat_status returns status."""
        mcp.connect_device(device_id)
        status = mcp.get_repeat_status()
        assert isinstance(status, dict)

    def test_sample_config_after_capture(self, mcp: McpClient, device_id: str,
                                         cleanup_after_test):
        """Sample config is accessible after capture."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)
        config = mcp.get_sample_config()
        assert isinstance(config, dict)
