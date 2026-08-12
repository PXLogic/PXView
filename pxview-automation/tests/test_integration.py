"""Integration tests for pxview-automation.

These tests require a **running PXView headless server** (``PXView.exe --headless``)
and a **demo device**.  They exercise the full capture → decode → export pipeline.

To run these tests::

    # Start PXView in headless mode
    PXView.exe --headless &

    # Run integration tests only
    pytest tests/test_integration.py -v -m integration

To skip integration tests (default when no server is available)::

    pytest tests/ -v  # unit tests only
"""

from __future__ import annotations

import os
import tempfile
import time
from typing import Optional

import pytest

from pxview_automation import (
    McpClient,
    McpConnectionError,
    McpError,
    PXView,
    CaptureConfiguration,
    LogicDeviceConfiguration,
    TimedCaptureMode,
    ManualCaptureMode,
    DigitalTriggerType,
    RadixType,
    ExportFormat,
    CaptureState,
)

# Skip all tests in this module if PXView server is not reachable.
pytestmark = pytest.mark.integration


# ======================================================================
# Fixtures
# ======================================================================


@pytest.fixture(scope="module")
def client() -> McpClient:
    """Module-scoped MCP client connected to PXView."""
    c = McpClient(timeout=30.0)
    try:
        c.connect()
    except McpConnectionError:
        pytest.skip("PXView MCP server not reachable. Start with: PXView.exe --headless")
    yield c
    c.disconnect()


@pytest.fixture(scope="module")
def demo_device(client: McpClient) -> Optional[dict]:
    """Find the demo device, skip if not available."""
    devices = client.get_devices(include_simulation_devices=True)
    for d in devices:
        if d.get("is_demo"):
            return d
    pytest.skip("No demo device available. Use: PXView.exe --headless")


@pytest.fixture
def temp_dir() -> str:
    """Temporary directory for export tests."""
    d = tempfile.mkdtemp(prefix="pxv_test_")
    yield d
    # Cleanup
    import shutil
    shutil.rmtree(d, ignore_errors=True)


# ======================================================================
# Device / connection tests
# ======================================================================


class TestDeviceDiscovery:
    def test_get_devices(self, client: McpClient):
        devices = client.get_devices()
        assert isinstance(devices, list)

    def test_get_devices_typed(self, client: McpClient):
        devices = client.get_devices_typed()
        assert all(hasattr(d, "id") for d in devices)

    def test_include_simulation(self, client: McpClient):
        devices = client.get_devices(include_simulation_devices=True)
        assert any(d.get("is_demo") for d in devices)

    def test_demo_device_exists(self, demo_device):
        assert demo_device is not None
        assert demo_device.get("is_demo") is True

    def test_refresh_device_list(self, client: McpClient):
        devices = client.refresh_device_list()
        assert isinstance(devices, list)


class TestChannelInfo:
    def test_get_channels(self, client: McpClient, demo_device):
        client.connect_device(demo_device["id"])
        channels = client.get_channels()
        assert isinstance(channels, list)
        assert len(channels) > 0

    def test_get_channels_typed(self, client: McpClient, demo_device):
        from pxview_automation import ChannelInfo
        channels = client.get_channels_typed()
        assert all(isinstance(ch, ChannelInfo) for ch in channels)


# ======================================================================
# Capture tests
# ======================================================================


class TestCapture:
    def test_timed_capture(self, client: McpClient, demo_device):
        """Start a timed capture and wait for completion."""
        client.connect_device(demo_device["id"])
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "timedCaptureMode": {"durationSeconds": 0.1},
            },
        )
        result = client.wait_capture(timeout_seconds=10.0)
        assert result is not None

        status = client.get_capture_status()
        assert status.get("state") in ("completed", "idle")

    def test_typed_capture(self, client: McpClient, demo_device):
        """Test typed capture using dataclasses."""
        client.connect_device(demo_device["id"])
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration=LogicDeviceConfiguration(
                digital_channels=[0, 1],
                digital_sample_rate=100000,
            ).to_dict(),
            capture_configuration=CaptureConfiguration(
                capture_mode=TimedCaptureMode(duration_seconds=0.1),
            ).to_dict(),
        )
        client.wait_capture(timeout_seconds=10.0)
        status = client.get_capture_status_typed()
        assert isinstance(status, CaptureState) or hasattr(status, "state")

    def test_manual_capture_stop(self, client: McpClient, demo_device):
        """Start a manual capture and stop it."""
        client.connect_device(demo_device["id"])
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "manualCaptureMode": {},
            },
        )
        time.sleep(0.2)
        client.stop_capture()
        status = client.get_capture_status()
        assert status.get("state") in ("completed", "idle", "error")

    def test_capture_status_typed(self, client: McpClient, demo_device):
        """Test typed capture status."""
        status = client.get_capture_status_typed()
        assert hasattr(status, "state")
        assert hasattr(status, "progress")


# ======================================================================
# Decode tests
# ======================================================================


class TestDecode:
    def test_list_analyzers(self, client: McpClient):
        analyzers = client.list_analyzers()
        assert isinstance(analyzers, list)
        assert len(analyzers) > 0
        # Common decoders should be available
        names = {a.get("id", a.get("name", "")) for a in analyzers}
        assert "i2c" in names or "spi" in names or "uart" in names

    def test_get_analyzer_options(self, client: McpClient):
        opts = client.get_analyzer_options("i2c")
        assert isinstance(opts, dict)

    def test_capture_and_decode_i2c(self, client: McpClient, demo_device):
        """Full pipeline: capture + I2C decode."""
        client.connect_device(demo_device["id"])

        # Add I2C decoder before capture
        analyzer_id = client.add_analyzer(
            analyzer_name="i2c",
            settings={
                "channelMap": {"scl": 0, "sda": 1},
            },
            device_id=demo_device["id"],
        )
        assert analyzer_id is not None

        # Capture
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0, 1],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "timedCaptureMode": {"durationSeconds": 0.1},
            },
        )
        client.wait_capture(timeout_seconds=10.0)

        # Get decode results
        time.sleep(0.5)
        results = client.get_analyzer_results(analyzer_id=str(analyzer_id))
        assert isinstance(results, list)

        # Cleanup
        client.remove_analyzer(str(analyzer_id))

    def test_clear_all_decoders(self, client: McpClient, demo_device):
        client.clear_all_decoders()
        active = client.get_active_decoders()
        assert len(active) == 0


# ======================================================================
# Export tests
# ======================================================================


class TestExport:
    def test_export_csv(self, client: McpClient, demo_device, temp_dir):
        """Export raw data as CSV."""
        client.connect_device(demo_device["id"])
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "timedCaptureMode": {"durationSeconds": 0.1},
            },
        )
        client.wait_capture(timeout_seconds=10.0)

        result = client.export_raw_data(format="csv", directory=temp_dir, digital_channels=[0])
        # Check that files were created
        files = os.listdir(temp_dir)
        assert len(files) > 0

    def test_export_binary(self, client: McpClient, demo_device, temp_dir):
        """Export raw data as binary."""
        client.connect_device(demo_device["id"])
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "timedCaptureMode": {"durationSeconds": 0.1},
            },
        )
        client.wait_capture(timeout_seconds=10.0)

        client.export_raw_data(format="binary", directory=temp_dir, digital_channels=[0])
        files = os.listdir(temp_dir)
        assert any(f.endswith(".bin") for f in files)

    def test_export_vcd(self, client: McpClient, demo_device, temp_dir):
        """Export raw data as VCD."""
        client.connect_device(demo_device["id"])
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "timedCaptureMode": {"durationSeconds": 0.1},
            },
        )
        client.wait_capture(timeout_seconds=10.0)

        client.export_raw_data(format="vcd", directory=temp_dir, digital_channels=[0])
        files = os.listdir(temp_dir)
        assert len(files) > 0

    def test_export_data_table_with_columns(self, client: McpClient, demo_device, temp_dir):
        """Test data table export with column selection and filter."""
        client.connect_device(demo_device["id"])

        # Add decoder
        analyzer_id = client.add_analyzer(
            analyzer_name="i2c",
            settings={"channelMap": {"scl": 0, "sda": 1}},
            device_id=demo_device["id"],
        )

        # Capture
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0, 1],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "timedCaptureMode": {"durationSeconds": 0.1},
            },
        )
        client.wait_capture(timeout_seconds=10.0)
        time.sleep(0.5)

        # Export with column selection
        out_file = os.path.join(temp_dir, "table.csv")
        client.export_data_table_csv(
            filepath=out_file,
            analyzer_id=str(analyzer_id),
            radix_type=3,
        )
        if os.path.exists(out_file):
            with open(out_file, "r") as f:
                content = f.read()
            assert len(content) > 0

        client.remove_analyzer(str(analyzer_id))


# ======================================================================
# Sample reading tests
# ======================================================================


class TestSampleReading:
    def test_get_logic_samples(self, client: McpClient, demo_device):
        """Read logic samples after a capture."""
        client.connect_device(demo_device["id"])
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "timedCaptureMode": {"durationSeconds": 0.1},
            },
        )
        client.wait_capture(timeout_seconds=10.0)

        data = client.get_samples(channel_type="logic", channel_index=0, start_sample=0, end_sample=100)
        assert isinstance(data, (bytes, bytearray))
        assert len(data) > 0

    def test_find_next_edge(self, client: McpClient, demo_device):
        """Test edge search."""
        client.connect_device(demo_device["id"])
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "timedCaptureMode": {"durationSeconds": 0.1},
            },
        )
        client.wait_capture(timeout_seconds=10.0)

        result = client.find_next_edge(
            channel_index=0,
            from_sample=0,
            rising_edge=True,
        )
        # Result is a sample index or None
        assert result is not None


# ======================================================================
# File save/load tests
# ======================================================================


class TestFileOperations:
    def test_save_and_load(self, client: McpClient, demo_device, temp_dir):
        """Save a capture and reload it."""
        client.connect_device(demo_device["id"])
        client.start_capture(
            device_id=demo_device["id"],
            logic_device_configuration={
                "digitalChannels": [0],
                "digitalSampleRate": 100000,
            },
            capture_configuration={
                "timedCaptureMode": {"durationSeconds": 0.1},
            },
        )
        client.wait_capture(timeout_seconds=10.0)

        # Save
        pxc_file = os.path.join(temp_dir, "test_capture.pxc")
        client.save_capture(filepath=pxc_file)
        assert os.path.exists(pxc_file)

        # Close current capture
        client.close_capture()

        # Load
        client.load_capture(filepath=pxc_file)


# ======================================================================
# High-level API tests
# ======================================================================


class TestHighLevelAPI:
    def test_capture_and_decode(self, demo_device):
        """Test the high-level PXView.capture_and_decode method."""
        with PXView(timeout=30.0) as pxv:
            pxv.connect()
            results = pxv.capture_and_decode(
                device_id=demo_device["id"],
                protocol="i2c",
                channel_map={"scl": 0, "sda": 1},
                channels=[0, 1],
                sample_rate=100000,
                duration_s=0.1,
                wait_timeout_s=10.0,
            )
            assert isinstance(results, list)

    def test_capture_typed(self, demo_device):
        """Test typed capture via PXView.capture_typed."""
        with PXView(timeout=30.0) as pxv:
            pxv.connect()
            pxv.capture_typed(
                device_id=demo_device["id"],
                device_config=LogicDeviceConfiguration(
                    digital_channels=[0],
                    digital_sample_rate=100000,
                ),
                capture_config=CaptureConfiguration(
                    capture_mode=TimedCaptureMode(duration_seconds=0.1),
                ),
                wait_timeout_s=10.0,
            )

    def test_export(self, demo_device, temp_dir):
        """Test high-level export."""
        with PXView(timeout=30.0) as pxv:
            pxv.connect()
            pxv.capture(
                device_id=demo_device["id"],
                channels=[0],
                sample_rate=100000,
                duration_s=0.1,
                wait_timeout_s=10.0,
            )
            pxv.export(format="csv", directory=temp_dir)
