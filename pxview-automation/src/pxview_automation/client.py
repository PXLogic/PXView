"""Low-level MCP client for PXView.

This module wraps all 46 MCP tools exposed by PXView's JSON-RPC 2.0
over HTTP API on port 10110.  It provides automatic JSON-RPC
encapsulation/parsing, error detection, retry logic, SSE stream
parsing, and base64 sample decoding.

Typical usage::

    from pxview_automation import McpClient

    client = McpClient()                    # defaults to 127.0.0.1:10110
    client.connect()
    devices = client.get_devices()
    print(devices)
    client.disconnect()

Or as a context manager::

    with McpClient() as client:
        client.connect()
        devices = client.get_devices()
"""

from __future__ import annotations

import base64
import json
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional

from ._utils import to_windows_path
from .exceptions import McpConnectionError, McpError
from .types import (
    AppInfo,
    CaptureStatus,
    ChannelInfo,
    DataTableExportConfiguration,
    DataTableFilter,
    DeviceDesc,
    ProbeConfig,
    SampleConfig,
)

# ------------------------------------------------------------------
# Force-disable all HTTP proxies at module import time.
#
# On Windows (especially GitHub Actions runners), Python's urllib
# falls back to getproxies_registry() when HTTP_PROXY is not set in
# the environment.  This reads the Windows registry for proxy settings,
# and the NO_PROXY environment variable is NOT consulted in that code
# path (only proxy_bypass_environment checks NO_PROXY).  As a result,
# requests to 127.0.0.1 may be routed through a system proxy and fail.
#
# Installing an opener with an empty ProxyHandler({}) at module import
# time guarantees that urlopen() never uses a proxy, regardless of
# registry or environment settings.
# ------------------------------------------------------------------
_no_proxy_handler = urllib.request.ProxyHandler({})
_urllib_opener = urllib.request.build_opener(_no_proxy_handler)
urllib.request.install_opener(_urllib_opener)


class McpClient:
    """MCP JSON-RPC 2.0 client for PXView.

    Wraps all 46 MCP tools.  Each tool is exposed as a Python method
    with the same name (snake_case).  Methods return parsed Python
    objects (dict / list / str / bytes / None).

    Args:
        url:         MCP endpoint URL.
        timeout:     Default HTTP timeout in seconds for each request.
        max_retries: Number of retries on connection failure.
        retry_delay: Delay between retries in seconds.
        auto_connect: If True, call :meth:`connect` in ``__init__``.

    Attributes:
        url:         MCP endpoint URL.
        timeout:     Default HTTP timeout in seconds.
        max_retries: Number of retries on connection failure.
        retry_delay: Delay between retries in seconds.
    """

    # ---- Construction & context manager ----

    def __init__(
        self,
        url: str = "http://127.0.0.1:10110/mcp",
        timeout: float = 60.0,
        max_retries: int = 3,
        retry_delay: float = 0.5,
        *,
        auto_connect: bool = False,
    ):
        self.url = url
        self.timeout = timeout
        self.max_retries = max_retries
        self.retry_delay = retry_delay
        self._request_id = 0
        self._connected = False
        self._tools: List[Dict[str, Any]] = []

        # Proxy bypass is handled at module level via ProxyHandler({})
        # (see top of file).  The NO_PROXY env var approach was insufficient
        # on Windows because urllib falls back to registry proxy settings.

        if auto_connect:
            self.connect()

    def __enter__(self) -> "McpClient":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.disconnect()

    def __repr__(self) -> str:
        status = "connected" if self._connected else "disconnected"
        return f"McpClient(url={self.url!r}, {status})"

    # ==================================================================
    # Low-level transport
    # ==================================================================

    def _next_id(self) -> int:
        self._request_id += 1
        return self._request_id

    @staticmethod
    def _parse_sse_response(text: str) -> dict:
        """Parse an SSE (Server-Sent Events) response body and extract
        the final 'result' event's JSON payload.

        SSE format::

            event: progress
            data: {"status":"capturing","elapsed_seconds":0.0}

            event: result
            data: {"jsonrpc":"2.0","id":1,"result":{...}}

        Returns the parsed JSON from the last 'result' event.
        Falls back to parsing the entire text as JSON if no SSE format
        detected.
        """
        if not text.startswith("event:"):
            return json.loads(text)

        result_json: Optional[dict] = None
        current_event: Optional[str] = None
        current_data_lines: list[str] = []

        for line in text.split("\n"):
            if line.startswith("event:"):
                current_event = line[6:].strip()
            elif line.startswith("data:"):
                current_data_lines.append(line[5:].strip())
            elif line.strip() == "":
                if current_event and current_data_lines:
                    data_str = "\n".join(current_data_lines)
                    try:
                        parsed = json.loads(data_str)
                        if current_event == "result":
                            result_json = parsed
                    except (json.JSONDecodeError, ValueError):
                        pass
                current_event = None
                current_data_lines = []

        # Handle last event (if no trailing empty line)
        if current_event and current_data_lines:
            data_str = "\n".join(current_data_lines)
            try:
                parsed = json.loads(data_str)
                if current_event == "result":
                    result_json = parsed
            except (json.JSONDecodeError, ValueError):
                pass

        if result_json is not None:
            return result_json

        raise McpConnectionError("SSE response did not contain a 'result' event")

    def _post(self, body: dict, timeout: Optional[float] = None) -> dict:
        """Send a JSON-RPC request and return the parsed response.

        On connection failure, attempts an automatic reconnection
        (re-handshake) before giving up, so that a server restart or
        brief network glitch doesn't permanently break the client.
        """
        t = timeout if timeout is not None else min(self.timeout, 30.0)
        raw = json.dumps(body).encode("utf-8")
        last_err: Optional[Exception] = None
        reconnected = False

        for attempt in range(self.max_retries):
            try:
                req = urllib.request.Request(
                    self.url,
                    data=raw,
                    headers={
                        "Content-Type": "application/json",
                        "Accept": "application/json, text/event-stream",
                        "Connection": "close",
                    },
                    method="POST",
                )
                with urllib.request.urlopen(req, timeout=t) as resp:
                    text = resp.read().decode("utf-8", errors="replace")
                    if not text.strip():
                        raise McpConnectionError(
                            f"Empty response from {self.url}"
                        )
                    content_type = resp.headers.get("Content-Type", "")
                    if "text/event-stream" in content_type:
                        return self._parse_sse_response(text)
                    return json.loads(text)
            except urllib.error.HTTPError as exc:
                try:
                    text = exc.read().decode("utf-8", errors="replace")
                    return json.loads(text)
                except Exception:
                    last_err = exc
            except McpConnectionError:
                raise
            except Exception as exc:
                last_err = exc
                if attempt < self.max_retries - 1:
                    # If the server might have restarted, try
                    # re-handshake once before retrying.
                    if not reconnected and self._connected:
                        reconnected = True
                        self._connected = False
                        try:
                            self.connect()
                        except Exception:
                            pass
                    time.sleep(self.retry_delay * (attempt + 1))
        raise McpConnectionError(
            f"Cannot connect to MCP server at {self.url}: {last_err}"
        )

    def _call_method(
        self,
        method: str,
        params: Optional[dict] = None,
        timeout: Optional[float] = None,
    ) -> dict:
        """Send a raw JSON-RPC method call."""
        body: Dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": method,
        }
        if params is not None:
            body["params"] = params
        return self._post(body, timeout=timeout)

    def _call_tool(
        self,
        name: str,
        arguments: Optional[dict] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Call an MCP tool and return the parsed result.

        Raises:
            McpError: if the tool returns an error.
        """
        params: Dict[str, Any] = {"name": name}
        if arguments is not None:
            params["arguments"] = arguments
        resp = self._call_method("tools/call", params, timeout=timeout)
        return self._parse_tool_result(resp)

    def _parse_tool_result(self, resp: dict) -> Any:
        """Parse a tools/call response into Python objects."""
        if "error" in resp:
            err = resp["error"]
            raise McpError(
                err.get("message", "Unknown error"),
                err.get("code", -1),
                err,
            )
        result = resp.get("result", {})
        if isinstance(result, dict) and result.get("isError"):
            content = result.get("content", [])
            text = content[0].get("text", "") if content else ""
            raise McpError(text)
        content = result.get("content") if isinstance(result, dict) else None
        if content and isinstance(content, list) and len(content) > 0:
            text = content[0].get("text", "")
            try:
                return json.loads(text)
            except (json.JSONDecodeError, ValueError):
                return text
        return result

    # ==================================================================
    # Connection management
    # ==================================================================

    def connect(self) -> None:
        """Initialize MCP connection: initialize -> list tools.

        Raises:
            McpError: if the initialize handshake fails.
            McpConnectionError: if the server cannot be reached.
        """
        resp = self._call_method(
            "initialize",
            {
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": "pxview-automation", "version": "1.5.5"},
            },
        )
        result = resp.get("result", resp) if isinstance(resp, dict) else {}
        if not isinstance(result, dict) or "protocolVersion" not in result:
            raise McpError(f"Initialize failed: {resp}")
        self._call_method("notifications/initialized", {})
        tools_resp = self._call_method("tools/list", {})
        tools_result = (
            tools_resp.get("result", tools_resp)
            if isinstance(tools_resp, dict)
            else {}
        )
        self._tools = (
            tools_result.get("tools", [])
            if isinstance(tools_result, dict)
            else []
        )
        self._connected = True

    def disconnect(self) -> None:
        """Disconnect from the MCP server."""
        self._connected = False

    @property
    def connected(self) -> bool:
        """True if the client has completed the MCP handshake."""
        return self._connected

    @property
    def tools(self) -> List[Dict[str, Any]]:
        """List of tool schemas discovered during connect()."""
        return self._tools

    @property
    def tool_names(self) -> List[str]:
        """List of tool names discovered during connect()."""
        return [t["name"] for t in self._tools]

    def dump_schema(self, filepath: Optional[str] = None) -> dict:
        """Dump the server's tool schemas to a JSON file.

        This extracts the schema from the MCP ``tools/list`` response
        (the single source of truth — ``tool_schemas.inc`` in the
        PXView C++ code) and writes it as a standalone JSON document.

        This replaces a hand-maintained static schema file: instead
        of keeping a separate ``mcp-schema.json`` in sync with the
        server, you generate it on demand from the running server.

        Args:
            filepath: Output file path.  If None, returns the schema
                      dict without writing a file.

        Returns:
            The schema as a dict (also written to *filepath* if given).

        Example::

            client.connect()
            client.dump_schema("mcp-schema.json")

            # Or from the CLI:
            # pxview-cli dump-schema --out mcp-schema.json
        """
        import json as _json

        schema = {
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "title": "PXView MCP Automation API Schema",
            "description": (
                "Auto-generated from the running PXView server's "
                "tools/list response. Source of truth: "
                "PXView/pv/api/tool_schemas.inc"
            ),
            "version": __import__("pxview_automation").__version__,
            "protocolVersion": "2025-03-26",
            "transport": "JSON-RPC 2.0 over HTTP",
            "defaultEndpoint": self.url,
            "toolCount": len(self._tools),
            "tools": {
                t["name"]: {
                    "description": t.get("description", ""),
                    "inputSchema": t.get("inputSchema", {}),
                }
                for t in self._tools
            },
        }

        if filepath is not None:
            with open(filepath, "w", encoding="utf-8") as f:
                _json.dump(schema, f, indent=2, ensure_ascii=False)
                f.write("\n")

        return schema

    def ping(self) -> bool:
        """Send a ping and return True if server responds."""
        try:
            self._call_method("ping", {})
            return True
        except McpError:
            return False

    def wait_for_server(
        self, timeout: float = 60.0, interval: float = 1.0
    ) -> bool:
        """Wait until the MCP server is reachable.

        Useful after launching ``PXView.exe --headless`` — the MCP
        transport may need a few seconds to start listening.

        Args:
            timeout:  Maximum wait time in seconds.
            interval: Polling interval in seconds.

        Returns:
            True if the server became reachable, False on timeout.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                raw = json.dumps(
                    {"jsonrpc": "2.0", "id": 0, "method": "ping"}
                ).encode("utf-8")
                req = urllib.request.Request(
                    self.url,
                    data=raw,
                    headers={
                        "Content-Type": "application/json",
                        "Connection": "close",
                    },
                    method="POST",
                )
                with urllib.request.urlopen(req, timeout=5.0) as resp:
                    text = resp.read().decode("utf-8", errors="replace")
                    if text.strip():
                        return True
            except Exception:
                pass
            time.sleep(interval)
        return False

    # ==================================================================
    # Tool wrappers — all 61 MCP tools
    # ==================================================================

    # ---- 1. Device Management (2 tools) ----

    def get_devices(
        self,
        include_simulation_devices: Optional[bool] = None,
        timeout: Optional[float] = None,
    ) -> List[dict]:
        """List connected devices.

        Call this first to discover available devices and their IDs
        before starting a capture.

        Args:
            include_simulation_devices: If True, include demo/simulation devices.

        Returns:
            List of device dicts, each containing:
            ``id``, ``driver_name``, ``display_name``, ``path``,
            ``is_hardware``, ``is_demo``, ``is_file``, etc.
        """
        args: dict = {}
        if include_simulation_devices is not None:
            args["includeSimulationDevices"] = include_simulation_devices
        return self._call_tool("get_devices", args, timeout=timeout)

    def get_devices_typed(
        self,
        include_simulation_devices: Optional[bool] = None,
        timeout: Optional[float] = None,
    ) -> List[DeviceDesc]:
        """List connected devices as typed :class:`DeviceDesc` objects.

        This is the typed equivalent of :meth:`get_devices`.
        """
        raw = self.get_devices(
            include_simulation_devices=include_simulation_devices,
            timeout=timeout,
        )
        return [DeviceDesc.from_dict(d) for d in raw]

    def get_channels(self, timeout: Optional[float] = None) -> List[dict]:
        """Get channel list for the current device.

        Returns:
            List of channel dicts: ``index``, ``name``, ``type``,
            ``enabled``, ``enabled_default``.
        """
        return self._call_tool("get_channels", {}, timeout=timeout)

    def get_channels_typed(self, timeout: Optional[float] = None) -> List[ChannelInfo]:
        """Get channel list as typed :class:`ChannelInfo` objects."""
        raw = self.get_channels(timeout=timeout)
        return [ChannelInfo.from_dict(d) for d in raw]

    def get_config(
        self,
        key: int,
        type: str,
        timeout: Optional[float] = None,
    ) -> Any:
        """Read a generic SR_CONF_* config value by key.

        Args:
            key:  SR_CONF_* config key (numeric).
            type: Value type: ``'bool'``, ``'int'``, ``'int64'``,
                  ``'string'``, ``'double'``, ``'uint64'``.

        Returns:
            ``{"value": ...}`` dict with the config value.
        """
        return self._call_tool(
            "get_config",
            {"key": key, "type": type},
            timeout=timeout,
        )

    def set_config(
        self,
        key: int,
        type: str,
        value: Any,
        timeout: Optional[float] = None,
    ) -> Any:
        """Write a generic SR_CONF_* config value by key.

        Args:
            key:   SR_CONF_* config key (numeric).
            type:  Value type: ``'bool'``, ``'int'``, ``'int64'``,
                   ``'string'``, ``'double'``, ``'uint64'``.
            value: Value to set (type depends on ``type`` field).
        """
        return self._call_tool(
            "set_config",
            {"key": key, "type": type, "value": value},
            timeout=timeout,
        )

    # ---- 2. Capture Control (4 tools) ----

    def start_capture(
        self,
        device_id: Optional[str] = None,
        logic_device_configuration: Optional[dict] = None,
        capture_configuration: Optional[dict] = None,
        timeout: Optional[float] = None,
        **extra: Any,
    ) -> Any:
        """Start a new signal capture.

        Args:
            device_id:                  Device ID (from :meth:`get_devices`).
            logic_device_configuration: Device/channel config dict with
                ``digitalChannels``, ``analogChannels``,
                ``digitalSampleRate``, ``digitalThresholdVolts``, etc.
            capture_configuration:      Capture mode config with
                ``timedCaptureMode``, ``manualCaptureMode``,
                ``digitalCaptureMode``, etc.

        Note:
            In Stream mode (``channelMode='Stream'``), duration and
            sample count are ignored — use :meth:`stop_capture` to end.
        """
        args: dict = {}
        if device_id is not None:
            args["deviceId"] = device_id
        # Flatten logic_device_configuration into top-level params.
        # The C++ start_capture tool expects flat params (digitalChannels,
        # digitalSampleRate, etc.), not nested logicDeviceConfiguration.
        if logic_device_configuration is not None:
            args.update(logic_device_configuration)
        # Convert capture_configuration from nested format to flat params.
        # Nested: {"timedCaptureMode": {"durationSeconds": 0.5}}
        # Flat:   {"captureMode": "timed", "durationSeconds": 0.5}
        if capture_configuration is not None:
            if "timedCaptureMode" in capture_configuration:
                args["captureMode"] = "timed"
                timed = capture_configuration["timedCaptureMode"]
                if isinstance(timed, dict) and "durationSeconds" in timed:
                    args["durationSeconds"] = timed["durationSeconds"]
            elif "manualCaptureMode" in capture_configuration:
                args["captureMode"] = "manual"
                manual = capture_configuration["manualCaptureMode"]
                if isinstance(manual, dict) and "sampleCount" in manual:
                    args["sampleCount"] = manual["sampleCount"]
            elif "streamCaptureMode" in capture_configuration:
                args["captureMode"] = "stream"
            else:
                # Unknown format — flatten as-is
                args.update(capture_configuration)
        args.update(extra)
        return self._call_tool("start_capture", args, timeout=timeout)

    def stop_capture(self, timeout: Optional[float] = None) -> Any:
        """Stop the active capture."""
        return self._call_tool("stop_capture", {}, timeout=timeout)

    def wait_capture(
        self,
        timeout_seconds: float = 300.0,
        timeout: Optional[float] = None,
    ) -> Any:
        """Wait for the current capture to complete.

        Blocks until the capture finishes or times out.  Uses SSE
        streaming internally to receive progress events.

        Args:
            timeout_seconds: Maximum wait time in seconds (server-side).
            timeout:         HTTP timeout (client-side). Defaults to
                             ``timeout_seconds + 10``.
        """
        return self._call_tool(
            "wait_capture",
            {"timeoutSeconds": timeout_seconds},
            timeout=timeout,
        )

    def get_capture_status(self, timeout: Optional[float] = None) -> dict:
        """Get current capture status and progress.

        Returns:
            Dict with ``state`` (idle/capturing/completed/paused/error),
            ``progress``, ``triggered``, etc.
        """
        return self._call_tool("get_capture_status", {}, timeout=timeout)

    def get_capture_status_typed(self, timeout: Optional[float] = None) -> CaptureStatus:
        """Get capture status as a typed :class:`CaptureStatus` object."""
        raw = self.get_capture_status(timeout=timeout)
        return CaptureStatus.from_dict(raw)

    # ---- 3. File Operations (3 tools) ----

    def load_capture(
        self, filepath: str, timeout: Optional[float] = None
    ) -> Any:
        """Load a capture from a ``.pxc`` session file.

        Args:
            filepath: Path to the capture file.  MSYS2-style paths
                      (``/c/Users/...``) are auto-converted to Windows.
        """
        return self._call_tool(
            "load_capture",
            {"filePath": to_windows_path(filepath)},
            timeout=timeout,
        )

    def save_capture(
        self, filepath: str, timeout: Optional[float] = None
    ) -> Any:
        """Save current capture to a ``.pxc`` session file.

        Args:
            filepath: Output file path.  MSYS2-style paths are
                      auto-converted to Windows.
        """
        return self._call_tool(
            "save_capture",
            {"filePath": to_windows_path(filepath)},
            timeout=timeout,
        )

    def close_capture(self, timeout: Optional[float] = None) -> Any:
        """Close current capture and free resources."""
        return self._call_tool("close_capture", {}, timeout=timeout)

    # ---- 4. Protocol Decoding (5 tools) ----

    def list_analyzers(self, timeout: Optional[float] = None) -> List[dict]:
        """List all available protocol analyzers/decoders.

        Returns:
            List of dicts: ``id``, ``name``, ``long_name``,
            ``channels``, ``optional_channels``.
        """
        return self._call_tool("list_analyzers", {}, timeout=timeout)

    def get_analyzer_options(
        self, analyzer_name: str, timeout: Optional[float] = None
    ) -> dict:
        """Get channel and option requirements for an analyzer.

        Args:
            analyzer_name: Decoder ID (e.g. ``'i2c'``, ``'spi'``, ``'uart'``).

        Returns:
            Dict describing required/optional channels and options.
        """
        return self._call_tool(
            "get_analyzer_options",
            {"decoderId": analyzer_name},
            timeout=timeout,
        )

    def add_analyzer(
        self,
        analyzer_name: str,
        settings: Optional[dict] = None,
        device_id: Optional[str] = None,
        analyzer_label: Optional[str] = None,
        stack_on_analyzer_id: Optional[str] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Add a protocol analyzer/decoder.

        Best called **before** :meth:`start_capture` so auto-decode
        triggers on capture completion.

        Args:
            analyzer_name:         Decoder ID (e.g. ``'i2c'``).
            settings:              Channel map + options dict.
            device_id:             Connect this device first (headless).
            analyzer_label:        Custom display label.
            stack_on_analyzer_id:  Stack on an existing analyzer.

        Returns:
            Analyzer instance ID string (e.g. ``'1:1'``).
        """
        # If device_id is provided, ensure the device is connected first.
        # The MCP add_analyzer tool does not have a deviceId parameter;
        # device connection must be done via connect_device beforehand.
        if device_id is not None:
            try:
                self.connect_device(device_id)
            except Exception:
                pass  # Already connected

        args: dict = {"decoderId": analyzer_name}
        # Flatten settings into top-level channelMap and options
        if settings is not None:
            if "channelMap" in settings:
                args["channelMap"] = settings["channelMap"]
            if "options" in settings:
                args["options"] = settings["options"]
        if analyzer_label is not None:
            args["label"] = analyzer_label
        if stack_on_analyzer_id is not None:
            args["stackOnAnalyzerId"] = stack_on_analyzer_id
        return self._call_tool("add_analyzer", args, timeout=timeout)

    def remove_analyzer(
        self, analyzer_id: str, timeout: Optional[float] = None
    ) -> Any:
        """Remove a protocol analyzer.

        Args:
            analyzer_id: Analyzer instance ID from :meth:`add_analyzer`.
        """
        return self._call_tool(
            "remove_analyzer",
            {"analyzerId": analyzer_id},
            timeout=timeout,
        )

    def get_analyzer_results(
        self,
        analyzer_id: str,
        start_sample: Optional[int] = None,
        end_sample: Optional[int] = None,
        max_count: int = 1000,
        timeout: Optional[float] = None,
    ) -> List[dict]:
        """Get protocol analyzer decoded annotations.

        Args:
            analyzer_id:  Analyzer instance ID.
            start_sample: Start sample index (0-based).
            end_sample:   End sample index (exclusive).
            max_count:    Maximum annotations to return.

        Returns:
            List of annotation dicts: ``ann_class``, ``ann_class_id``,
            ``data``, ``start_sample``, ``end_sample``, ``row_index``.
        """
        args: dict = {"analyzerId": analyzer_id, "maxCount": max_count}
        if start_sample is not None:
            args["startSample"] = start_sample
        if end_sample is not None:
            args["endSample"] = end_sample
        return self._call_tool(
            "get_analyzer_results", args, timeout=timeout
        )

    def export_raw_data(
        self,
        format: str,
        directory: str,
        digital_channels: Optional[List[int]] = None,
        analog_channels: Optional[List[int]] = None,
        analog_downsample_ratio: int = 1,
        iso8601_timestamp: bool = False,
        timeout: Optional[float] = None,
    ) -> Any:
        """Export raw capture data in a chosen format.

        Args:
            format: ``'csv'``, ``'binary'``, ``'vcd'``, ``'hex'``, ``'bits'``.
            directory: Output directory.
        """
        args: dict = {
            "format": format,
            "directory": to_windows_path(directory),
        }
        if digital_channels is not None:
            args["digitalChannels"] = digital_channels
        if analog_channels is not None:
            args["analogChannels"] = analog_channels
        args["analogDownsampleRatio"] = analog_downsample_ratio
        args["iso8601Timestamp"] = iso8601_timestamp
        return self._call_tool("export_raw_data", args, timeout=timeout)

    def export_data_table_csv(
        self,
        filepath: str,
        analyzer_id: Optional[str] = None,
        *,
        radix_type: int = 0,
        analyzers: Optional[List[dict]] = None,
        iso8601_timestamp: bool = False,
        timeout: Optional[float] = None,
    ) -> Any:
        """Export decoded analyzer results as a CSV data table.

        Single mode:
            export_data_table_csv("out.csv", analyzer_id="1:1", radix_type=3)

        Multi mode:
            export_data_table_csv("out", analyzers=[
                {"analyzerId": "1:1", "radixType": 3},
                {"analyzerId": "1:2", "radixType": 1},
            ])
            # Generates out_1_1.csv and out_1_2.csv

        Args:
            filepath:          Output CSV file path (or prefix for multi mode).
            analyzer_id:       Single mode: analyzer instance ID.
            radix_type:        Radix: 1=Binary, 2=Decimal, 3=Hex, 4=Ascii.
            analyzers:         Multi mode: list of ``{analyzerId, radixType}`` dicts.
            iso8601_timestamp: Use ISO8601 wall-clock timestamps.
        """
        args: dict = {
            "filePath": to_windows_path(filepath),
            "iso8601Timestamp": iso8601_timestamp,
        }
        if analyzers is not None:
            args["analyzers"] = analyzers
        else:
            if analyzer_id is None:
                raise ValueError("Provide either 'analyzers' or 'analyzer_id'")
            args["analyzerId"] = analyzer_id
            args["radixType"] = radix_type
        return self._call_tool("export_data_table_csv", args, timeout=timeout)

    def get_sample_config(self, timeout: Optional[float] = None) -> dict:
        """Get the full sample configuration.

        Uses get_session_status(include='config') internally.

        Returns: ``sample_rate``, ``sample_limit``, ``time_base``,
        ``collect_mode``, ``stream_mode``, ``rle_enabled``,
        ``repeat_interval``, ``repeat_hold_percent``.
        """
        result = self._call_tool(
            "get_session_status", {"include": "config"}, timeout=timeout)
        if isinstance(result, dict) and "sampleConfig" in result:
            return result["sampleConfig"]
        return result

    def get_sample_config_typed(self, timeout: Optional[float] = None) -> SampleConfig:
        """Get sample configuration as a typed :class:`SampleConfig` object."""
        raw = self.get_sample_config(timeout=timeout)
        return SampleConfig.from_dict(raw)

    def find_next_edge(
        self,
        channel_index: int,
        from_sample: int,
        rising_edge: bool = True,
        timeout: Optional[float] = None,
    ) -> Any:
        """Find the next signal edge on a channel.

        Args:
            channel_index: Digital channel index.
            from_sample:   Sample index to start searching from.
            rising_edge:   True = find rising edge, False = find falling edge.

        Returns:
            Sample index of the next edge, or None.
        """
        return self._call_tool(
            "find_next_edge",
            {
                "channelIndex": channel_index,
                "fromSample": from_sample,
                "risingEdge": rising_edge,
            },
            timeout=timeout,
        )

    def find_pattern(
        self,
        from_sample: int,
        *,
        channel_index: Optional[int] = None,
        pattern: Optional[str] = None,
        channels: Optional[List[dict]] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Search for a signal pattern.

        Single-channel mode (simple)::

            find_pattern(from_sample=0, channel_index=0, pattern="1")

        Multi-channel mode (combined)::

            find_pattern(from_sample=0, channels=[
                {"channelIndex": 0, "state": "1"},
                {"channelIndex": 1, "state": "0"},
            ])

        Args:
            from_sample:   Sample index to start from.
            channel_index: Single mode: channel index.
            pattern:       Single mode: pattern string (``'1'``, ``'0'``, ``'x'``).
            channels:      Multi mode: list of ``{channelIndex, state}`` dicts.

        Returns:
            Sample index of the first match, or None.
        """
        args: dict = {"fromSample": from_sample}
        if channels is not None:
            args["channels"] = channels
        else:
            if channel_index is None or pattern is None:
                raise ValueError(
                    "Provide either 'channels' or 'channel_index'+'pattern'")
            args["channelIndex"] = channel_index
            args["pattern"] = pattern
        return self._call_tool("find_pattern", args, timeout=timeout)

    # ---- 12. Decoder Management (2 tools) ----

    def get_active_decoders(
        self, timeout: Optional[float] = None
    ) -> List[dict]:
        """List all currently active decoder instances."""
        return self._call_tool("get_active_decoders", {}, timeout=timeout)

    def clear_all_decoders(self, timeout: Optional[float] = None) -> Any:
        """Remove all active decoder instances."""
        return self._call_tool("clear_all_decoders", {}, timeout=timeout)

    # ---- 13. Session Management (5 tools) ----

    def list_sessions(self, timeout: Optional[float] = None) -> List[dict]:
        """List all sessions."""
        return self._call_tool("list_sessions", {}, timeout=timeout)

    def create_session(
        self,
        name: Optional[str] = None,
        device_id: Optional[str] = None,
        file_path: Optional[str] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Create a new session.  Returns the session ID."""
        args: dict = {}
        if name is not None:
            args["name"] = name
        if device_id is not None:
            args["deviceId"] = device_id
        if file_path is not None:
            args["filePath"] = file_path
        return self._call_tool("create_session", args, timeout=timeout)

    def destroy_session(
        self, session_id: int, timeout: Optional[float] = None
    ) -> Any:
        """Destroy a session by ID."""
        return self._call_tool(
            "destroy_session", {"sessionId": session_id}, timeout=timeout
        )

    def set_active_session(
        self, session_id: int, timeout: Optional[float] = None
    ) -> Any:
        """Switch the active session."""
        return self._call_tool(
            "set_active_session", {"sessionId": session_id}, timeout=timeout
        )

    def connect_device(
        self, device_id: str, timeout: Optional[float] = None
    ) -> Any:
        """Connect to a device by ID.

        Creates a new session bound to that device if no session exists.
        Waits 1 second after connection for the server to settle.
        """
        result = self._call_tool(
            "connect_device", {"deviceId": device_id}, timeout=timeout
        )
        time.sleep(1)
        return result

    def disconnect_device(
        self, device_id: Optional[str] = None, timeout: Optional[float] = None
    ) -> Any:
        """Disconnect the active or specified device."""
        args: dict = {}
        if device_id is not None:
            args["deviceId"] = device_id
        return self._call_tool("disconnect_device", args, timeout=timeout)

    def refresh_device_list(
        self, timeout: Optional[float] = None
    ) -> List[dict]:
        """Trigger a hot-plug rescan and return updated device list.

        Uses a 120-second default timeout because scanning all drivers
        can take a long time on systems with many USB devices.
        """
        return self._call_tool(
            "refresh_device_list", {}, timeout=timeout or 120.0
        )

    def set_export_config(
        self,
        start_sample: int,
        end_sample: int,
        timeout: Optional[float] = None,
    ) -> Any:
        """Set the export/display range (in samples).

        Note: These are export/display offsets, not acquisition triggers.
        """
        return self._call_tool(
            "set_export_config",
            {"startSample": start_sample, "endSample": end_sample},
            timeout=timeout,
        )

    def reconfigure_decoder(
        self,
        analyzer_id: str,
        options: Optional[dict] = None,
        channel_map: Optional[dict] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Reconfigure an existing decoder's options and channel_map
        in place (no remove + re-add)."""
        args: dict = {"analyzerId": analyzer_id}
        if options is not None:
            args["options"] = options
        if channel_map is not None:
            args["channelMap"] = channel_map
        return self._call_tool("reconfigure_decoder", args, timeout=timeout)

    def get_measurement_results(
        self,
        types: Optional[List[str]] = None,
        timeout: Optional[float] = None,
    ) -> dict:
        """Get measurement and analysis results.

        Args:
            types: List of result types to include: 'math', 'spectrum', 'lissajous'.
                  If None, returns all available.

        Returns:
            Dict with keys for each requested type.
        """
        args: dict = {}
        if types is not None:
            args["types"] = types
        return self._call_tool("get_measurement_results", args, timeout=timeout)

    def get_decoder_class_names(
        self,
        decoder_name: str,
        timeout: Optional[float] = None,
    ) -> List[dict]:
        """Get annotation class names for a decoder type.

        Convenience method: temporarily adds a decoder instance, queries
        class names via ``get_analyzer_results(includeMetadata=true)``,
        then removes the decoder.

        Args:
            decoder_name: Decoder ID (e.g. ``'i2c_c'``, ``'spi_c'``).

        Returns:
            List of ``{class_id, class_name}`` dicts, or empty list
            if the decoder cannot be queried.
        """
        # Build a channel map from the decoder's options first: the server
        # rejects add_analyzer when required channels are unmapped ("Required
        # channel(s) not mapped"), so the old assumption that decoders can be
        # added without a channelMap no longer holds. Map each declared
        # channel to its own index (0, 1, 2, ...).
        channel_map: dict = {}
        try:
            opts = self._call_tool(
                "get_analyzer_options", {"decoderId": decoder_name},
                timeout=timeout,
            )
            if isinstance(opts, dict):
                for i, ch in enumerate(opts.get("channels") or []):
                    ch_id = (
                        ch.get("id")
                        or ch.get("name")
                        or ch.get("idn")
                        or f"ch{i}"
                    )
                    channel_map[ch_id] = i
        except (McpError, Exception):
            pass

        try:
            args: dict = {"decoderId": decoder_name}
            if channel_map:
                args["channelMap"] = channel_map
            result = self._call_tool("add_analyzer", args, timeout=timeout)
            analyzer_id = None
            if isinstance(result, dict):
                analyzer_id = (
                    result.get("analyzerId")
                    or result.get("instance_id")
                    or result.get("id")
                )
            elif isinstance(result, str):
                analyzer_id = result
            if not analyzer_id:
                return []
        except (McpError, Exception):
            return []

        try:
            result = self._call_tool(
                "get_analyzer_results",
                {
                    "analyzerId": analyzer_id,
                    "includeMetadata": True,
                    "maxCount": 1,
                },
                timeout=timeout,
            )
            if isinstance(result, dict) and "metadata" in result:
                return result["metadata"].get("classNames", [])
            return []
        finally:
            try:
                self.remove_analyzer(analyzer_id)
            except Exception:
                pass

    def get_demo_device(self) -> dict:
        """Find and return the demo device from :meth:`get_devices`.

        Raises:
            McpError: if no demo device is found.
        """
        devices = self.get_devices()
        for d in devices:
            if d.get("is_demo"):
                return d
        raise McpError("No demo device found in device list")

    def get_hardware_device(self) -> Optional[dict]:
        """Find and return the first non-demo hardware device, or None."""
        devices = self.get_devices()
        for d in devices:
            if not d.get("is_demo") and d.get("is_hardware"):
                return d
        return None

    def safe_capture_and_wait(
        self,
        device_id: str,
        logic_config: Optional[dict] = None,
        capture_config: Optional[dict] = None,
        wait_timeout: float = 60.0,
    ) -> Any:
        """Convenience: start_capture + wait_capture with cleanup on error.

        If :meth:`wait_capture` fails, :meth:`stop_capture` is called
        before re-raising the exception.
        """
        try:
            self.start_capture(device_id, logic_config, capture_config)
            return self.wait_capture(
                timeout_seconds=wait_timeout,
                timeout=wait_timeout + 10,
            )
        except Exception:
            try:
                self.stop_capture()
            except Exception:
                pass
            raise

    # ---- 17. Cursors (4 tools) ----

    def get_cursors(self, timeout: Optional[float] = None) -> List[dict]:
        """Get all cursor positions.

        Returns a list of dicts with ``sample_position`` and
        ``index`` fields.

        Uses the consolidated ``configure_cursors`` tool with action='get'.
        """
        return self._call_tool(
            "configure_cursors", {"action": "get"}, timeout=timeout
        )

    def add_cursor(self, sample_pos: int, timeout: Optional[float] = None) -> Any:
        """Add a cursor at the given sample position.

        Uses the consolidated ``configure_cursors`` tool with action='add'.
        """
        return self._call_tool(
            "configure_cursors",
            {"action": "add", "samplePos": sample_pos},
            timeout=timeout,
        )

    def remove_cursor(self, index: int, timeout: Optional[float] = None) -> Any:
        """Remove the cursor at the given index.

        Uses the consolidated ``configure_cursors`` tool with action='remove'.
        """
        return self._call_tool(
            "configure_cursors",
            {"action": "remove", "index": index},
            timeout=timeout,
        )

    def clear_cursors(self, timeout: Optional[float] = None) -> Any:
        """Remove all cursors.

        Uses the consolidated ``configure_cursors`` tool with action='clear'.
        """
        return self._call_tool(
            "configure_cursors", {"action": "clear"}, timeout=timeout
        )

    # ================================================================
    # 18. Consolidated Tools (Phase 0-8 — 46 tools replacing 64)
    # ================================================================

    # ---- Tier 0: Mode Management (3 tools) ----

    def switch_work_mode(self, mode: int, timeout: Optional[float] = None) -> Any:
        """Switch the device work mode.

        Modes: 0=Logic, 1=DSO, 2=Analog, 3=MSO.
        Must be called before configuring channels/triggers/probes.
        """
        return self._call_tool("switch_work_mode", {"mode": mode}, timeout=timeout)

    def get_work_mode(self, timeout: Optional[float] = None) -> int:
        """Get the current device work mode (0=Logic, 1=DSO, 2=Analog, 3=MSO)."""
        result = self._call_tool("get_work_mode", {}, timeout=timeout)
        if isinstance(result, dict):
            return result.get("mode", -1)
        return result

    def get_supported_work_modes(self, timeout: Optional[float] = None) -> List[int]:
        """Get the work modes supported by the current device."""
        result = self._call_tool("get_supported_work_modes", {}, timeout=timeout)
        if isinstance(result, dict):
            return result.get("modes", [])
        return result

    # ---- Tier 2: Consolidated Configuration ----

    def set_sample_config(
        self,
        sample_rate: Optional[int] = None,
        sample_limit: Optional[int] = None,
        time_base: Optional[int] = None,
        collect_mode: Optional[int] = None,
        repeat_interval: Optional[float] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Set sample configuration (consolidated).

        All params optional — only provided params are updated.
        Sample rate type is determined by current work mode.
        """
        args: Dict[str, Any] = {}
        if sample_rate is not None:
            args["sampleRate"] = sample_rate
        if sample_limit is not None:
            args["sampleLimit"] = sample_limit
        if time_base is not None:
            args["timeBase"] = time_base
        if collect_mode is not None:
            args["collectMode"] = collect_mode
        if repeat_interval is not None:
            args["repeatInterval"] = repeat_interval
        return self._call_tool("set_sample_config", args, timeout=timeout)

    def configure_channel(
        self,
        channel_index: int,
        enabled: Optional[bool] = None,
        name: Optional[str] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Configure a channel (consolidated set_channel_enabled + set_channel_name)."""
        args: Dict[str, Any] = {"channelIndex": channel_index}
        if enabled is not None:
            args["enabled"] = enabled
        if name is not None:
            args["name"] = name
        return self._call_tool("configure_channel", args, timeout=timeout)

    def configure_trigger(
        self,
        stage_count: Optional[int] = None,
        config_json: Optional[str] = None,
        source: Optional[int] = None,
        slope: Optional[int] = None,
        horiz_pos: Optional[float] = None,
        holdoff: Optional[float] = None,
        margin: Optional[float] = None,
        channel: Optional[int] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Get or set trigger configuration (consolidated, mode-aware).

        Automatically uses LogicTrigger or DsoTrigger based on current mode.
        Call with no args to get current config.
        """
        args: Dict[str, Any] = {}
        if stage_count is not None:
            args["stageCount"] = stage_count
        if config_json is not None:
            args["configJson"] = config_json
        if source is not None:
            args["source"] = source
        if slope is not None:
            args["slope"] = slope
        if horiz_pos is not None:
            args["horizPos"] = horiz_pos
        if holdoff is not None:
            args["holdoff"] = holdoff
        if margin is not None:
            args["margin"] = margin
        if channel is not None:
            args["channel"] = channel
        return self._call_tool("configure_trigger", args, timeout=timeout)

    def configure_probe(
        self,
        channel_index: int,
        vdiv: Optional[float] = None,
        coupling: Optional[int] = None,
        vfactor: Optional[float] = None,
        map_default: Optional[bool] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Get or set probe configuration (consolidated, DSO/Analog/MSO only)."""
        args: Dict[str, Any] = {"channelIndex": channel_index}
        if vdiv is not None:
            args["vdiv"] = vdiv
        if coupling is not None:
            args["coupling"] = coupling
        if vfactor is not None:
            args["vfactor"] = vfactor
        if map_default is not None:
            args["mapDefault"] = map_default
        return self._call_tool("configure_probe", args, timeout=timeout)

    def configure_glitch_filter(
        self,
        channels: Optional[List[int]] = None,
        thresholds: Optional[List[int]] = None,
        modes: Optional[List[int]] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Get, set, or clear glitch filter (consolidated, Logic/MSO only).

        Call with no args to get current config.
        Set channels=[] to clear.
        """
        args: Dict[str, Any] = {}
        if channels is not None:
            args["channels"] = channels
        if thresholds is not None:
            args["thresholds"] = thresholds
        if modes is not None:
            args["modes"] = modes
        return self._call_tool("configure_glitch_filter", args, timeout=timeout)

    def configure_signal_invert(
        self,
        channels: Optional[List[int]] = None,
        invert_states: Optional[List[bool]] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Get, set, or clear signal invert (consolidated)."""
        args: Dict[str, Any] = {}
        if channels is not None:
            args["channels"] = channels
        if invert_states is not None:
            args["invertStates"] = invert_states
        return self._call_tool("configure_signal_invert", args, timeout=timeout)

    def get_session_status(self, timeout: Optional[float] = None) -> Any:
        """Get session status (consolidated repeat_status + disk_cache_info)."""
        return self._call_tool("get_session_status", {}, timeout=timeout)

    def configure_error_state(
        self, action: str = "get", timeout: Optional[float] = None
    ) -> Any:
        """Get or clear the session error state (consolidated)."""
        return self._call_tool(
            "configure_error_state", {"action": action}, timeout=timeout
        )

    # ---- Tier 3: Consolidated Sample Reading ----

    def get_samples(
        self,
        channel_index: int,
        channel_type: str,
        start_sample: int = 0,
        end_sample: Optional[int] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Read raw samples (consolidated get_logic/analog/dso_samples).

        channelType must match current work mode:
        'logic' for Logic/MSO, 'analog' for Analog, 'dso' for DSO.

        For logic channels, returns decoded bytes (one byte per sample).
        For analog/DSO channels, returns a list of float values.
        """
        args: Dict[str, Any] = {
            "channelIndex": channel_index,
            "channelType": channel_type,
            "startSample": start_sample,
        }
        if end_sample is not None:
            args["endSample"] = end_sample
        result = self._call_tool("get_samples", args, timeout=timeout)
        # Extract data from the {sample_count, data, encoding} response
        if isinstance(result, dict) and "data" in result:
            data = result["data"]
            if channel_type == "logic" and isinstance(data, str):
                return base64.b64decode(data)
            return data
        return result

    # ---- Generic Device Config (SR_CONF_* keys) ----

    def get_config(
        self,
        key: int,
        type: str,
        timeout: Optional[float] = None,
    ) -> Any:
        """Read a generic SR_CONF_* config value by key.

        Args:
            key: SR_CONF_* config key (numeric integer).
            type: Value type — ``'bool'``, ``'int'``, ``'int64'``,
                  ``'string'``, ``'double'``, ``'uint64'``.
        """
        return self._call_tool(
            "get_config",
            {"key": key, "type": type},
            timeout=timeout,
        )

    def set_config(
        self,
        key: int,
        type: str,
        value: Any,
        timeout: Optional[float] = None,
    ) -> Any:
        """Write a generic SR_CONF_* config value by key.

        Args:
            key: SR_CONF_* config key (numeric integer).
            type: Value type — ``'bool'``, ``'int'``, ``'int64'``,
                  ``'string'``, ``'double'``, ``'uint64'``.
            value: Value to set (type depends on ``type``).
        """
        return self._call_tool(
            "set_config",
            {"key": key, "type": type, "value": value},
            timeout=timeout,
        )
