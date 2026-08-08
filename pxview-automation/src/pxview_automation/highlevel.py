"""High-level API for PXView automation.

This module wraps :class:`~pxview_automation.client.McpClient` with
domain-level methods that combine multiple MCP tools into single
business operations.  Instead of calling ``set_sample_rate`` →
``set_channel_enabled`` → ``start_capture`` → ``wait_capture``
individually, you call :meth:`PXView.capture` in one line.

Typical usage::

    from pxview_automation import PXView

    with PXView() as pxv:
        pxv.connect()
        device = pxv.find_device(demo=True)

        # One-line capture + decode + export
        pxv.capture_and_decode(
            device_id=device["id"],
            channels=[0, 1],
            sample_rate=1_000_000,
            duration_s=1.0,
            protocol="i2c",
            channel_map={"scl": 0, "sda": 1},
        )
        pxv.export(format="csv", directory="./output")
"""

from __future__ import annotations

import time
from typing import Any, Dict, List, Optional

from .client import McpClient
from .exceptions import ConfigError, McpError
from ._utils import to_windows_path


class PXView:
    """High-level PXView automation API.

    Wraps :class:`McpClient` with convenience methods that combine
    multiple MCP tool calls into single business operations.

    Args:
        host: MCP server hostname (default: ``'127.0.0.1'``).
        port: MCP server port (default: ``10110``).
        timeout: Default HTTP timeout in seconds.
        auto_connect: If True, call :meth:`connect` in ``__init__``.

    Example::

        with PXView() as pxv:
            pxv.connect()
            devices = pxv.list_devices()
            pxv.capture(device_id="demo", channels=[0], sample_rate=1e6, duration_s=0.5)
    """

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 10110,
        timeout: float = 60.0,
        *,
        auto_connect: bool = False,
    ):
        self._client = McpClient(
            url=f"http://{host}:{port}/mcp",
            timeout=timeout,
        )
        self._host = host
        self._port = port

        if auto_connect:
            self.connect()

    def __enter__(self) -> "PXView":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.disconnect()

    @property
    def client(self) -> McpClient:
        """Access the underlying low-level :class:`McpClient`.

        Use this for MCP tools that don't have a high-level wrapper.
        """
        return self._client

    @property
    def connected(self) -> bool:
        return self._client.connected

    def connect(self) -> None:
        """Connect to the MCP server and complete the handshake."""
        self._client.connect()

    def disconnect(self) -> None:
        """Disconnect from the MCP server."""
        self._client.disconnect()

    # ==================================================================
    # Device discovery
    # ==================================================================

    def list_devices(
        self, include_sim: bool = False
    ) -> List[dict]:
        """List all connected devices.

        Args:
            include_sim: Include demo/simulation devices.

        Returns:
            List of device dicts.
        """
        return self._client.get_devices(
            include_simulation_devices=include_sim
        )

    def find_device(
        self,
        demo: bool = False,
        hardware: bool = False,
        driver: Optional[str] = None,
    ) -> Optional[dict]:
        """Find a device matching the given criteria.

        Args:
            demo:     If True, return the first demo device.
            hardware: If True, return the first real hardware device.
            driver:   Filter by driver name (e.g. ``'fx2lafw'``).

        Returns:
            Device dict, or None if no matching device is found.
        """
        devices = self._client.get_devices()
        for d in devices:
            if demo and not d.get("is_demo"):
                continue
            if hardware and not d.get("is_hardware"):
                continue
            if driver and d.get("driver_name") != driver:
                continue
            return d
        return None

    def scan_devices(self) -> List[dict]:
        """Trigger a hot-plug rescan and return the updated device list."""
        return self._client.refresh_device_list()

    def get_demo_device(self) -> dict:
        """Return the demo device.  Raises if not found."""
        return self._client.get_demo_device()

    # ==================================================================
    # Capture
    # ==================================================================

    def capture(
        self,
        device_id: str,
        *,
        channels: Optional[List[int]] = None,
        analog_channels: Optional[List[int]] = None,
        sample_rate: Optional[int] = None,
        duration_s: Optional[float] = None,
        sample_count: Optional[int] = None,
        threshold_v: Optional[float] = None,
        instant: bool = False,
        trigger_channel: Optional[int] = None,
        trigger_type: Optional[str] = None,
        wait: bool = True,
        wait_timeout_s: float = 300.0,
    ) -> dict:
        """Configure and start a capture, optionally waiting for completion.

        This is the primary high-level capture method.  It wraps
        ``connect_device`` + ``set_channel_enabled`` + ``set_sample_rate``
        + ``start_capture`` + ``wait_capture`` into a single call.

        Args:
            device_id:       Device ID from :meth:`list_devices`.
            channels:        List of digital channel indices to enable.
            analog_channels: List of analog channel indices to enable.
            sample_rate:     Sample rate in Hz.
            duration_s:      Capture duration in seconds (timed mode).
            sample_count:    Number of samples to capture (manual mode).
                             If both ``duration_s`` and ``sample_count``
                             are given, ``duration_s`` takes precedence.
            threshold_v:     Digital threshold voltage.
            instant:         If True, use instant (no-trigger) capture.
            trigger_channel: Digital channel index for trigger.
            trigger_type:    Trigger type: ``'rising'``, ``'falling'``,
                             ``'pulse_high'``, ``'pulse_low'``.
            wait:            If True, block until capture completes.
            wait_timeout_s:  Wait timeout in seconds.

        Returns:
            Capture status dict (from :meth:`get_capture_status`).
        """
        # Build logic device configuration
        logic_cfg: Dict[str, Any] = {}
        if channels is not None:
            logic_cfg["digitalChannels"] = channels
        if analog_channels is not None:
            logic_cfg["analogChannels"] = analog_channels
        if sample_rate is not None:
            logic_cfg["digitalSampleRate"] = sample_rate
        if threshold_v is not None:
            logic_cfg["digitalThresholdVolts"] = threshold_v

        # Build capture configuration
        cap_cfg: Dict[str, Any] = {}
        if duration_s is not None:
            cap_cfg["timedCaptureMode"] = {"durationSeconds": duration_s}
        elif sample_count is not None:
            cap_cfg["manualCaptureMode"] = {"sampleCount": sample_count}

        # Build trigger configuration
        if trigger_channel is not None and trigger_type is not None:
            cap_cfg["digitalCaptureMode"] = {
                "triggerChannelIndex": trigger_channel,
                "triggerType": trigger_type,
            }

        # Start capture
        self._client.start_capture(
            device_id=device_id,
            logic_device_configuration=logic_cfg if logic_cfg else None,
            capture_configuration=cap_cfg if cap_cfg else None,
        )

        if not wait:
            return self._client.get_capture_status()

        # Wait for completion
        self._client.wait_capture(
            timeout_seconds=wait_timeout_s,
            timeout=wait_timeout_s + 10,
        )

        status = self._client.get_capture_status()
        if status.get("state") == "error":
            err = self._client.get_error_state()
            raise McpError(
                f"Capture failed: {err.get('error_message', 'unknown error')}",
                raw=err,
            )

        return status

    def capture_and_wait(
        self,
        device_id: str,
        *,
        channels: Optional[List[int]] = None,
        sample_rate: Optional[int] = None,
        duration_s: Optional[float] = None,
        sample_count: Optional[int] = None,
        wait_timeout_s: float = 300.0,
    ) -> dict:
        """Alias for :meth:`capture` with ``wait=True`` (default)."""
        return self.capture(
            device_id,
            channels=channels,
            sample_rate=sample_rate,
            duration_s=duration_s,
            sample_count=sample_count,
            wait=True,
            wait_timeout_s=wait_timeout_s,
        )

    def stop_capture(self) -> Any:
        """Stop the active capture."""
        return self._client.stop_capture()

    def get_status(self) -> dict:
        """Get current capture status."""
        return self._client.get_capture_status()

    # ==================================================================
    # Decoding
    # ==================================================================

    def list_decoders(self) -> List[dict]:
        """List all available protocol decoders."""
        return self._client.list_analyzers()

    def add_decoder(
        self,
        protocol: str,
        channel_map: Optional[Dict[str, int]] = None,
        options: Optional[Dict[str, str]] = None,
        *,
        device_id: Optional[str] = None,
        label: Optional[str] = None,
        stack_on: Optional[str] = None,
    ) -> str:
        """Add a protocol decoder and return its instance ID.

        Args:
            protocol:    Decoder ID (e.g. ``'i2c'``, ``'spi'``, ``'uart'``).
            channel_map: Mapping of decoder channel names to device
                         channel indices (e.g. ``{'scl': 0, 'sda': 1}``).
            options:     Decoder-specific options (e.g. ``{'baudrate': '115200'}``).
            device_id:   Connect this device first (for headless mode).
            label:       Custom display label.
            stack_on:    Stack on an existing analyzer instance ID.

        Returns:
            Analyzer instance ID string (e.g. ``'1:1'``).
        """
        settings: Dict[str, Any] = {}
        if channel_map:
            settings["channelMap"] = channel_map
        if options:
            settings["options"] = options

        result = self._client.add_analyzer(
            analyzer_name=protocol,
            settings=settings if settings else None,
            device_id=device_id,
            analyzer_label=label,
            stack_on_analyzer_id=stack_on,
        )

        # The result is typically {"analyzerId": "1:1"} or just the ID string
        if isinstance(result, dict):
            return str(result.get("analyzerId", result.get("instance_id", "")))
        if isinstance(result, str):
            return result
        return str(result)

    def get_decoder_results(
        self,
        analyzer_id: str,
        *,
        max_count: int = 1000,
    ) -> List[dict]:
        """Get decoded annotations from a protocol analyzer.

        Args:
            analyzer_id: Analyzer instance ID from :meth:`add_decoder`.
            max_count:   Maximum annotations to return.

        Returns:
            List of annotation dicts.
        """
        return self._client.get_analyzer_results(
            analyzer_id=analyzer_id,
            max_count=max_count,
        )

    def clear_decoders(self) -> Any:
        """Remove all active decoders."""
        return self._client.clear_all_decoders()

    def list_active_decoders(self) -> List[dict]:
        """List all currently active decoder instances."""
        return self._client.get_active_decoders()

    # ==================================================================
    # Capture + Decode (one-shot)
    # ==================================================================

    def capture_and_decode(
        self,
        device_id: str,
        protocol: str,
        channel_map: Dict[str, int],
        *,
        channels: Optional[List[int]] = None,
        sample_rate: Optional[int] = None,
        duration_s: Optional[float] = None,
        sample_count: Optional[int] = None,
        decoder_options: Optional[Dict[str, str]] = None,
        wait_timeout_s: float = 300.0,
    ) -> List[dict]:
        """Capture + decode in one call.

        Workflow:
        1. Add the protocol decoder (before capture, for auto-decode).
        2. Start capture with given parameters.
        3. Wait for capture completion (auto-decode runs after).
        4. Return decoded annotations.

        Args:
            device_id:       Device ID.
            protocol:        Decoder ID (e.g. ``'i2c'``).
            channel_map:     Decoder channel mapping.
            channels:        Digital channels to enable.
            sample_rate:     Sample rate in Hz.
            duration_s:      Capture duration in seconds.
            sample_count:    Number of samples (alternative to duration).
            decoder_options: Decoder-specific options.
            wait_timeout_s:  Wait timeout in seconds.

        Returns:
            List of decoded annotation dicts.
        """
        # Ensure channels include all mapped channels
        all_channels = set(channels or [])
        all_channels.update(channel_map.values())
        channels = sorted(all_channels)

        # Add decoder before capture (enables auto-decode)
        analyzer_id = self.add_decoder(
            protocol=protocol,
            channel_map=channel_map,
            options=decoder_options,
            device_id=device_id,
        )

        # Capture and wait
        self.capture(
            device_id,
            channels=channels,
            sample_rate=sample_rate,
            duration_s=duration_s,
            sample_count=sample_count,
            wait=True,
            wait_timeout_s=wait_timeout_s,
        )

        # Give the decoder a moment to finish
        time.sleep(0.5)

        # Return decoded results
        return self.get_decoder_results(analyzer_id)

    # ==================================================================
    # Export
    # ==================================================================

    def export(
        self,
        format: str = "csv",
        directory: str = ".",
        *,
        digital_channels: Optional[List[int]] = None,
        analog_channels: Optional[List[int]] = None,
        analog_downsample_ratio: int = 1,
        iso8601_timestamp: bool = False,
    ) -> Any:
        """Export raw capture data.

        Args:
            format:      ``'csv'``, ``'binary'``, ``'vcd'``, ``'hex'``, ``'bits'``.
            directory:   Output directory.
            digital_channels: Digital channel indices to export.
            analog_channels:  Analog channel indices to export.
        """
        return self._client.export_raw_data(
            format=format,
            directory=directory,
            digital_channels=digital_channels,
            analog_channels=analog_channels,
            analog_downsample_ratio=analog_downsample_ratio,
            iso8601_timestamp=iso8601_timestamp,
        )

    def export_decoder_table(
        self,
        filepath: str,
        analyzer_id: Optional[str] = None,
        *,
        iso8601_timestamp: bool = False,
    ) -> Any:
        """Export decoded analyzer results as a CSV data table.

        Args:
            filepath:    Output CSV file path.
            analyzer_id: Specific analyzer to export.  If None, exports all.
        """
        analyzers = None
        if analyzer_id is not None:
            analyzers = [{"analyzerId": analyzer_id}]

        return self._client.export_data_table_csv(
            filepath=filepath,
            analyzers=analyzers,
            iso8601_timestamp=iso8601_timestamp,
        )

    # ==================================================================
    # Sample reading
    # ==================================================================

    def get_logic_samples(
        self,
        channel: int,
        start: int = 0,
        count: Optional[int] = None,
    ) -> bytes:
        """Read logic samples for a channel.

        Args:
            channel: Digital channel index.
            start:   Start sample index (0-based).
            count:   Number of samples to read.  None = to end.

        Returns:
            Raw bytes (one byte per sample, 0 or 1).
        """
        end = start + count if count is not None else None
        return self._client.get_logic_samples(
            channel_index=channel,
            start_sample=start,
            end_sample=end,
        )

    def get_analog_samples(
        self,
        channel: int,
        start: int = 0,
        count: Optional[int] = None,
    ) -> List[float]:
        """Read analog samples for a channel.

        Returns a list of float values.
        """
        end = start + count if count is not None else None
        return self._client.get_analog_samples(
            channel_index=channel,
            start_sample=start,
            end_sample=end,
        )

    # ==================================================================
    # File operations
    # ==================================================================

    def load(self, filepath: str) -> Any:
        """Load a capture from a ``.pxc`` session file."""
        return self._client.load_capture(filepath)

    def save(self, filepath: str) -> Any:
        """Save current capture to a ``.pxc`` session file."""
        return self._client.save_capture(filepath)

    def close(self) -> Any:
        """Close current capture and free resources."""
        return self._client.close_capture()

    # ==================================================================
    # Config shortcuts
    # ==================================================================

    def get_sample_rate(self) -> int:
        """Get the current sample rate in Hz."""
        cfg = self._client.get_sample_config()
        return cfg.get("sample_rate", 0)

    def set_sample_rate(self, rate: int) -> Any:
        """Set the sample rate in Hz."""
        return self._client.set_sample_rate(rate)

    def get_channels(self) -> List[dict]:
        """Get channel list for the current device."""
        return self._client.get_channels()

    def enable_channel(self, index: int) -> Any:
        """Enable a channel."""
        return self._client.set_channel_enabled(index, True)

    def disable_channel(self, index: int) -> Any:
        """Disable a channel."""
        return self._client.set_channel_enabled(index, False)
