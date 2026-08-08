"""
mcp_client.py - MCP (Model Context Protocol) client library for PXView.

Wraps all 61 MCP tools exposed by PXView's JSON-RPC 2.0 over HTTP API.
Provides automatic JSON-RPC encapsulation/parsing, error detection,
retry logic, and response type conversion.

Usage:
    client = McpClient("http://127.0.0.1:10110/mcp")
    client.connect()
    devices = client.get_devices()
    client.start_capture(deviceId=devices[0]["id"], ...)
"""

from __future__ import annotations

import base64
import json
import os
import re
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional, Union

# ------------------------------------------------------------------
# Force-disable all HTTP proxies for this module.
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


def _to_windows_path(path: str) -> str:
    """Convert a MSYS2/Cygwin-style path to a Windows-native path.

    PXView is a native Windows application and cannot understand
    MSYS2 paths like /c/Users/... or /tmp/...  This function converts
    such paths to C:\\Users\\... or the equivalent Windows form.
    """
    if not path:
        return path
    # Convert /c/Users/... → C:/Users/...
    m = re.match(r'^/([a-zA-Z])/(.*)$', path)
    if m:
        return f"{m.group(1).upper()}:/{m.group(2)}"
    # Convert /tmp/... → use TEMP env var
    if path.startswith('/tmp/'):
        tmp = os.environ.get('TEMP', os.environ.get('TMP', 'C:\\Temp'))
        # TEMP is already in Windows format, just append the rest
        rest = path[5:]  # skip '/tmp/'
        return tmp.replace('\\\\', '/') + '/' + rest
    return path


class McpError(Exception):
    """Raised when an MCP tool call returns an error."""

    def __init__(self, message: str, code: int = -1, raw: Any = None):
        super().__init__(message)
        self.code = code
        self.raw = raw


class McpConnectionError(McpError):
    """Raised when the MCP server cannot be reached."""
    pass


class McpClient:
    """MCP JSON-RPC 2.0 client for PXView.

    Attributes:
        url: MCP endpoint URL.
        timeout: Default HTTP timeout in seconds.
        max_retries: Number of retries on connection failure.
        retry_delay: Delay between retries in seconds.
    """

    def __init__(
        self,
        url: str = "http://127.0.0.1:10110/mcp",
        timeout: float = 60.0,
        max_retries: int = 3,
        retry_delay: float = 0.5,
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

    # ---- Low-level transport ----

    def _next_id(self) -> int:
        self._request_id += 1
        return self._request_id

    @staticmethod
    def _parse_sse_response(text: str) -> dict:
        """Parse an SSE (Server-Sent Events) response body and extract
        the final 'result' event's JSON payload.

        SSE format:
            event: progress
            data: {"status":"capturing","elapsed_seconds":0.0}

            event: result
            data: {"jsonrpc":"2.0","id":1,"result":{...}}

        Returns the parsed JSON from the last 'result' event.
        Falls back to parsing the entire text as JSON if no SSE format detected.
        """
        if not text.startswith("event:"):
            # Not SSE — parse as regular JSON
            return json.loads(text)

        # Parse SSE events
        result_json: Optional[dict] = None
        current_event = None
        current_data_lines: list = []

        for line in text.split("\n"):
            if line.startswith("event:"):
                current_event = line[6:].strip()
            elif line.startswith("data:"):
                current_data_lines.append(line[5:].strip())
            elif line.strip() == "":
                # End of event
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

        raise McpConnectionError(
            "SSE response did not contain a 'result' event"
        )

    def _post(self, body: dict, timeout: Optional[float] = None) -> dict:
        """Send a JSON-RPC request and return the parsed response."""
        # Use a shorter default per-request timeout to avoid long hangs
        # when the server is temporarily unresponsive. Explicit timeouts
        # (e.g. for wait_capture) are respected as-is.
        t = timeout if timeout is not None else min(self.timeout, 30.0)
        raw = json.dumps(body).encode("utf-8")
        last_err: Optional[Exception] = None
        for attempt in range(self.max_retries):
            try:
                req = urllib.request.Request(
                    self.url, data=raw,
                    headers={"Content-Type": "application/json",
                             "Accept": "application/json, text/event-stream",
                             "Connection": "close"},
                    method="POST",
                )
                with urllib.request.urlopen(req, timeout=t) as resp:
                    text = resp.read().decode("utf-8", errors="replace")
                    if not text.strip():
                        # Empty response body — server may have timed out
                        # or closed connection prematurely
                        raise McpConnectionError(
                            f"Empty response from {self.url}"
                        )
                    # Check if response is SSE (wait_capture uses SSE streaming)
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
            except Exception as exc:
                last_err = exc
                if attempt < self.max_retries - 1:
                    time.sleep(self.retry_delay)
        raise McpConnectionError(
            f"Cannot connect to MCP server at {self.url}: {last_err}"
        )

    def _call_method(self, method: str, params: Optional[dict] = None,
                     timeout: Optional[float] = None) -> dict:
        """Send a raw JSON-RPC method call."""
        body: Dict[str, Any] = {"jsonrpc": "2.0", "id": self._next_id(),
                                "method": method}
        if params is not None:
            body["params"] = params
        return self._post(body, timeout=timeout)

    def _call_tool(self, name: str, arguments: Optional[dict] = None,
                   timeout: Optional[float] = None) -> Any:
        """Call an MCP tool and return the parsed result.

        Raises McpError if the tool returns an error.
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
        # Extract text from content array
        content = result.get("content") if isinstance(result, dict) else None
        if content and isinstance(content, list) and len(content) > 0:
            text = content[0].get("text", "")
            # Try to parse as JSON, fall back to raw text
            try:
                return json.loads(text)
            except (json.JSONDecodeError, ValueError):
                return text
        return result

    # ---- Connection management ----

    def connect(self) -> None:
        """Initialize MCP connection: initialize -> list tools."""
        resp = self._call_method("initialize", {
            "protocolVersion": "2025-03-26",
            "capabilities": {},
            "clientInfo": {"name": "pxview-test", "version": "1.0.0"},
        })
        # initialize returns result directly (not wrapped in content)
        result = resp.get("result", resp) if isinstance(resp, dict) else {}
        if not isinstance(result, dict) or "protocolVersion" not in result:
            raise McpError(f"Initialize failed: {resp}")
        # Send initialized notification (fire-and-forget)
        self._call_method("notifications/initialized", {})
        # List tools
        tools_resp = self._call_method("tools/list", {})
        tools_result = tools_resp.get("result", tools_resp) if isinstance(tools_resp, dict) else {}
        self._tools = tools_result.get("tools", []) if isinstance(tools_result, dict) else []
        self._connected = True

    def disconnect(self) -> None:
        self._connected = False

    @property
    def connected(self) -> bool:
        return self._connected

    @property
    def tools(self) -> List[Dict[str, Any]]:
        return self._tools

    @property
    def tool_names(self) -> List[str]:
        return [t["name"] for t in self._tools]

    def ping(self) -> bool:
        """Send a ping and return True if server responds."""
        try:
            self._call_method("ping", {})
            return True
        except McpError:
            return False

    def wait_for_server(self, timeout: float = 60.0,
                        interval: float = 1.0) -> bool:
        """Wait until the MCP server is reachable."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                # Use a raw HTTP POST to check connectivity — don't rely
                # on any specific JSON-RPC method being supported.
                raw = json.dumps({
                    "jsonrpc": "2.0", "id": 0, "method": "ping",
                }).encode("utf-8")
                req = urllib.request.Request(
                    self.url, data=raw,
                    headers={"Content-Type": "application/json",
                             "Connection": "close"},
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
    # Tool wrappers — all 61 tools
    # ==================================================================

    # ---- 1. Device Management (2 tools) ----

    def get_devices(self, include_simulation_devices: Optional[bool] = None,
                    timeout: Optional[float] = None) -> List[dict]:
        """List connected devices."""
        args: dict = {}
        if include_simulation_devices is not None:
            args["includeSimulationDevices"] = include_simulation_devices
        return self._call_tool("get_devices", args, timeout=timeout)

    def get_channels(self, timeout: Optional[float] = None) -> List[dict]:
        """Get channel list for current device."""
        return self._call_tool("get_channels", {}, timeout=timeout)

    # ---- 2. Capture Control (4 tools) ----

    def start_capture(self, device_id: Optional[str] = None,
                      logic_device_configuration: Optional[dict] = None,
                      capture_configuration: Optional[dict] = None,
                      timeout: Optional[float] = None,
                      **extra) -> Any:
        """Start a signal capture."""
        args: dict = {}
        if device_id is not None:
            args["deviceId"] = device_id
        if logic_device_configuration is not None:
            args["logicDeviceConfiguration"] = logic_device_configuration
        if capture_configuration is not None:
            args["captureConfiguration"] = capture_configuration
        args.update(extra)
        return self._call_tool("start_capture", args, timeout=timeout)

    def stop_capture(self, timeout: Optional[float] = None) -> Any:
        """Stop the active capture."""
        return self._call_tool("stop_capture", {}, timeout=timeout)

    def wait_capture(self, timeout_seconds: float = 300.0,
                     timeout: Optional[float] = None) -> Any:
        """Wait for the current capture to complete."""
        return self._call_tool("wait_capture", {
            "timeoutSeconds": timeout_seconds
        }, timeout=timeout)

    def get_capture_status(self, timeout: Optional[float] = None) -> dict:
        """Get current capture status."""
        return self._call_tool("get_capture_status", {}, timeout=timeout)

    # ---- 3. File Operations (3 tools) ----

    def load_capture(self, filepath: str,
                     timeout: Optional[float] = None) -> Any:
        """Load a capture from a .pxc session file."""
        return self._call_tool("load_capture", {
            "filepath": _to_windows_path(filepath)
        }, timeout=timeout)

    def save_capture(self, filepath: str,
                     timeout: Optional[float] = None) -> Any:
        """Save current capture to a .pxc session file."""
        return self._call_tool("save_capture", {
            "filepath": _to_windows_path(filepath)
        }, timeout=timeout)

    def close_capture(self, timeout: Optional[float] = None) -> Any:
        """Close current capture and free resources."""
        return self._call_tool("close_capture", {}, timeout=timeout)

    # ---- 4. Protocol Decoding (5 tools) ----

    def list_analyzers(self, timeout: Optional[float] = None) -> List[dict]:
        """List all available protocol analyzers."""
        return self._call_tool("list_analyzers", {}, timeout=timeout)

    def get_analyzer_options(self, analyzer_name: str,
                             timeout: Optional[float] = None) -> dict:
        """Get channel and option requirements for an analyzer."""
        return self._call_tool("get_analyzer_options", {
            "analyzerName": analyzer_name
        }, timeout=timeout)

    def add_analyzer(self, analyzer_name: str,
                     settings: Optional[dict] = None,
                     device_id: Optional[str] = None,
                     analyzer_label: Optional[str] = None,
                     stack_on_analyzer_id: Optional[str] = None,
                     timeout: Optional[float] = None) -> Any:
        """Add a protocol analyzer."""
        args: dict = {"analyzerName": analyzer_name}
        if settings is not None:
            args["settings"] = settings
        if device_id is not None:
            args["deviceId"] = device_id
        if analyzer_label is not None:
            args["analyzerLabel"] = analyzer_label
        if stack_on_analyzer_id is not None:
            args["stackOnAnalyzerId"] = stack_on_analyzer_id
        return self._call_tool("add_analyzer", args, timeout=timeout)

    def remove_analyzer(self, analyzer_id: str,
                        timeout: Optional[float] = None) -> Any:
        """Remove a protocol analyzer."""
        return self._call_tool("remove_analyzer", {
            "analyzerId": analyzer_id
        }, timeout=timeout)

    def get_analyzer_results(self, analyzer_id: str,
                             start_sample: Optional[int] = None,
                             end_sample: Optional[int] = None,
                             max_count: int = 1000,
                             timeout: Optional[float] = None) -> List[dict]:
        """Get protocol analyzer decoded annotations."""
        args: dict = {"analyzerId": analyzer_id, "maxCount": max_count}
        if start_sample is not None:
            args["startSample"] = start_sample
        if end_sample is not None:
            args["endSample"] = end_sample
        return self._call_tool("get_analyzer_results", args, timeout=timeout)

    # ---- 5. Data Export (3 tools) ----

    def export_raw_data_csv(self, directory: str,
                            digital_channels: Optional[List[int]] = None,
                            analog_channels: Optional[List[int]] = None,
                            analog_downsample_ratio: int = 1,
                            iso8601_timestamp: bool = False,
                            timeout: Optional[float] = None) -> Any:
        """Export raw capture data as CSV files."""
        args: dict = {"directory": _to_windows_path(directory)}
        if digital_channels is not None:
            args["digitalChannels"] = digital_channels
        if analog_channels is not None:
            args["analogChannels"] = analog_channels
        args["analogDownsampleRatio"] = analog_downsample_ratio
        args["iso8601Timestamp"] = iso8601_timestamp
        return self._call_tool("export_raw_data_csv", args, timeout=timeout)

    def export_raw_data_binary(self, directory: str,
                               digital_channels: Optional[List[int]] = None,
                               analog_channels: Optional[List[int]] = None,
                               analog_downsample_ratio: int = 1,
                               timeout: Optional[float] = None) -> Any:
        """Export raw capture data as binary files."""
        args: dict = {"directory": _to_windows_path(directory)}
        if digital_channels is not None:
            args["digitalChannels"] = digital_channels
        if analog_channels is not None:
            args["analogChannels"] = analog_channels
        args["analogDownsampleRatio"] = analog_downsample_ratio
        return self._call_tool("export_raw_data_binary", args, timeout=timeout)

    def export_raw_data(self, format: str, directory: str,
                        digital_channels: Optional[List[int]] = None,
                        analog_channels: Optional[List[int]] = None,
                        analog_downsample_ratio: int = 1,
                        iso8601_timestamp: bool = False,
                        timeout: Optional[float] = None) -> Any:
        """Export raw capture data in a chosen format: csv, binary, vcd, hex, bits."""
        args: dict = {"format": format,
                      "directory": _to_windows_path(directory)}
        if digital_channels is not None:
            args["digitalChannels"] = digital_channels
        if analog_channels is not None:
            args["analogChannels"] = analog_channels
        args["analogDownsampleRatio"] = analog_downsample_ratio
        args["iso8601Timestamp"] = iso8601_timestamp
        return self._call_tool("export_raw_data", args, timeout=timeout)

    def export_data_table_csv(self, filepath: str,
                              analyzers: Optional[List[dict]] = None,
                              iso8601_timestamp: bool = False,
                              timeout: Optional[float] = None) -> Any:
        """Export analyzer results as a CSV data table."""
        args: dict = {"filepath": _to_windows_path(filepath)}
        if analyzers is not None:
            args["analyzers"] = analyzers
        args["iso8601Timestamp"] = iso8601_timestamp
        return self._call_tool("export_data_table_csv", args, timeout=timeout)

    # ---- 6. Trigger Config (2 tools) ----

    def get_trigger_config(self, mode: Optional[str] = None,
                           timeout: Optional[float] = None) -> dict:
        """Get trigger configuration."""
        args: dict = {}
        if mode is not None:
            args["mode"] = mode
        return self._call_tool("get_trigger_config", args, timeout=timeout)

    def set_trigger_config(self, mode: str, **kwargs) -> Any:
        """Set trigger configuration."""
        args: dict = {"mode": mode}
        args.update(kwargs)
        return self._call_tool("set_trigger_config", args)

    # ---- 7. Probe Config (2 tools) ----

    def get_probe_config(self, channel_index: int,
                         timeout: Optional[float] = None) -> dict:
        """Get probe configuration for a channel."""
        return self._call_tool("get_probe_config", {
            "channelIndex": channel_index
        }, timeout=timeout)

    def set_probe_config(self, channel_index: int,
                         vdiv: Optional[float] = None,
                         coupling: Optional[int] = None,
                         vfactor: Optional[float] = None,
                         map_default: Optional[bool] = None,
                         timeout: Optional[float] = None) -> Any:
        """Set probe configuration for a channel."""
        args: dict = {"channelIndex": channel_index}
        if vdiv is not None:
            args["vdiv"] = vdiv
        if coupling is not None:
            args["coupling"] = coupling
        if vfactor is not None:
            args["vfactor"] = vfactor
        if map_default is not None:
            args["mapDefault"] = map_default
        return self._call_tool("set_probe_config", args, timeout=timeout)

    # ---- 8. Channel Config (2 tools) ----

    def set_channel_enabled(self, channel_index: int, enabled: bool,
                            timeout: Optional[float] = None) -> Any:
        """Enable or disable a channel."""
        return self._call_tool("set_channel_enabled", {
            "channelIndex": channel_index,
            "enabled": enabled
        }, timeout=timeout)

    def set_channel_name(self, channel_index: int, name: str,
                         timeout: Optional[float] = None) -> Any:
        """Rename a channel."""
        return self._call_tool("set_channel_name", {
            "channelIndex": channel_index,
            "name": name
        }, timeout=timeout)

    # ---- 9. Sample Config (5 tools) ----

    def get_sample_config(self, timeout: Optional[float] = None) -> dict:
        """Get full sample configuration."""
        return self._call_tool("get_sample_config", {}, timeout=timeout)

    def set_sample_rate(self, rate: int,
                        timeout: Optional[float] = None) -> Any:
        """Set the sample rate in Hz."""
        return self._call_tool("set_sample_rate", {"rate": rate},
                               timeout=timeout)

    def set_sample_limit(self, limit: int,
                         timeout: Optional[float] = None) -> Any:
        """Set the sample limit."""
        return self._call_tool("set_sample_limit", {"limit": limit},
                               timeout=timeout)

    def set_time_base(self, time_base: int,
                      timeout: Optional[float] = None) -> Any:
        """Set the time base in nanoseconds."""
        return self._call_tool("set_time_base", {"timeBase": time_base},
                               timeout=timeout)

    def set_collect_mode(self, mode: str,
                         timeout: Optional[float] = None) -> Any:
        """Set the collect mode: single, repetitive, or loop."""
        return self._call_tool("set_collect_mode", {"mode": mode},
                               timeout=timeout)

    def set_repeat_interval(self, interval_ms: int,
                            timeout: Optional[float] = None) -> Any:
        """Set the repeat interval in milliseconds."""
        return self._call_tool("set_repeat_interval", {
            "intervalMs": interval_ms
        }, timeout=timeout)

    # ---- 10. Sample Reading (3 tools) ----

    def get_logic_samples(self, channel_index: int,
                          start_sample: int = 0,
                          end_sample: Optional[int] = None,
                          timeout: Optional[float] = None) -> bytes:
        """Read logic samples for a channel. Returns raw bytes."""
        args: dict = {"channelIndex": channel_index,
                      "startSample": start_sample}
        if end_sample is not None:
            args["endSample"] = end_sample
        result = self._call_tool("get_logic_samples", args, timeout=timeout)
        # Result is base64-encoded string
        if isinstance(result, str):
            return base64.b64decode(result)
        if isinstance(result, dict) and "data" in result:
            return base64.b64decode(result["data"])
        return result

    def get_analog_samples(self, channel_index: int,
                           start_sample: int = 0,
                           end_sample: Optional[int] = None,
                           timeout: Optional[float] = None) -> List[float]:
        """Read analog samples for a channel."""
        args: dict = {"channelIndex": channel_index,
                      "startSample": start_sample}
        if end_sample is not None:
            args["endSample"] = end_sample
        return self._call_tool("get_analog_samples", args, timeout=timeout)

    def get_dso_samples(self, channel_index: int,
                        start_sample: int = 0,
                        end_sample: Optional[int] = None,
                        timeout: Optional[float] = None) -> List[float]:
        """Read DSO samples for a channel."""
        args: dict = {"channelIndex": channel_index,
                      "startSample": start_sample}
        if end_sample is not None:
            args["endSample"] = end_sample
        return self._call_tool("get_dso_samples", args, timeout=timeout)

    # ---- 11. Edge/Pattern Search (2 tools) ----

    def find_next_edge(self, channel_index: int, start_sample: int,
                       direction: str = "forward",
                       timeout: Optional[float] = None) -> Any:
        """Find the next logic edge on a channel."""
        return self._call_tool("find_next_edge", {
            "channelIndex": channel_index,
            "startSample": start_sample,
            "direction": direction
        }, timeout=timeout)

    def find_pattern(self, channels: List[int], pattern: str,
                     start_sample: int,
                     options: Optional[dict] = None,
                     timeout: Optional[float] = None) -> Any:
        """Search for a bit pattern on logic channels."""
        args: dict = {"channels": channels, "pattern": pattern,
                      "startSample": start_sample}
        if options is not None:
            args["options"] = options
        return self._call_tool("find_pattern", args, timeout=timeout)

    # ---- 12. Decoder Management (2 tools) ----

    def get_active_decoders(self, timeout: Optional[float] = None) -> List[dict]:
        """List all currently active decoder instances."""
        return self._call_tool("get_active_decoders", {}, timeout=timeout)

    def clear_all_decoders(self, timeout: Optional[float] = None) -> Any:
        """Remove all active decoder instances."""
        return self._call_tool("clear_all_decoders", {}, timeout=timeout)

    # ---- 13. Session Management (5 tools) ----

    def list_sessions(self, timeout: Optional[float] = None) -> List[dict]:
        """List all sessions."""
        return self._call_tool("list_sessions", {}, timeout=timeout)

    def create_session(self, name: Optional[str] = None,
                       device_id: Optional[str] = None,
                       file_path: Optional[str] = None,
                       timeout: Optional[float] = None) -> Any:
        """Create a new session."""
        args: dict = {}
        if name is not None:
            args["name"] = name
        if device_id is not None:
            args["deviceId"] = device_id
        if file_path is not None:
            args["filePath"] = file_path
        return self._call_tool("create_session", args, timeout=timeout)

    def destroy_session(self, session_id: int,
                        timeout: Optional[float] = None) -> Any:
        """Destroy a session by id."""
        return self._call_tool("destroy_session", {
            "sessionId": session_id
        }, timeout=timeout)

    def set_active_session(self, session_id: int,
                           timeout: Optional[float] = None) -> Any:
        """Switch the active session."""
        return self._call_tool("set_active_session", {
            "sessionId": session_id
        }, timeout=timeout)

    def get_session_count(self, timeout: Optional[float] = None) -> int:
        """Return the current number of sessions."""
        return self._call_tool("get_session_count", {}, timeout=timeout)

    # ---- 14. Device Connect/Disconnect (2 tools) ----

    def connect_device(self, device_id: str,
                       timeout: Optional[float] = None) -> Any:
        """Connect to a device by id."""
        result = self._call_tool("connect_device", {
            "deviceId": device_id
        }, timeout=timeout)
        # Device connection triggers init_signals and possibly a default
        # capture. Give the server a moment to settle before the next call.
        time.sleep(1)
        return result

    def disconnect_device(self, device_id: Optional[str] = None,
                          timeout: Optional[float] = None) -> Any:
        """Disconnect the active or specified device."""
        args: dict = {}
        if device_id is not None:
            args["deviceId"] = device_id
        return self._call_tool("disconnect_device", args, timeout=timeout)

    # ---- 15. Generic Config (2 tools) ----

    def get_config(self, key: int, value_type: str,
                   timeout: Optional[float] = None) -> Any:
        """Read a generic SR_CONF_* config value."""
        return self._call_tool("get_config", {
            "key": key, "type": value_type
        }, timeout=timeout)

    def set_config(self, key: int, value_type: str, value: Any,
                   timeout: Optional[float] = None) -> Any:
        """Write a generic SR_CONF_* config value."""
        return self._call_tool("set_config", {
            "key": key, "type": value_type, "value": value
        }, timeout=timeout)

    # ---- 16. Glitch Filter (3 tools) ----

    def set_glitch_filter(self, channels: List[int],
                          threshold: Optional[int] = None,
                          thresholds: Optional[List[int]] = None,
                          modes: Optional[List[int]] = None,
                          timeout: Optional[float] = None) -> Any:
        """Enable glitch filter on channels."""
        args: dict = {"channels": channels}
        if threshold is not None:
            args["threshold"] = threshold
        if thresholds is not None:
            args["thresholds"] = thresholds
        if modes is not None:
            args["modes"] = modes
        return self._call_tool("set_glitch_filter", args, timeout=timeout)

    def clear_glitch_filter(self,
                            channels: Optional[List[int]] = None,
                            timeout: Optional[float] = None) -> Any:
        """Clear glitch filter."""
        args: dict = {}
        if channels is not None:
            args["channels"] = channels
        return self._call_tool("clear_glitch_filter", args, timeout=timeout)

    def get_glitch_filter_config(self,
                                 timeout: Optional[float] = None) -> dict:
        """Get current glitch filter configuration."""
        return self._call_tool("get_glitch_filter_config", {},
                               timeout=timeout)

    # ---- 17. Signal Invert (3 tools) ----

    def set_signal_invert(self, channels: List[int],
                          timeout: Optional[float] = None) -> Any:
        """Enable signal invert on channels."""
        return self._call_tool("set_signal_invert", {
            "channels": channels
        }, timeout=timeout)

    def clear_signal_invert(self,
                            channels: Optional[List[int]] = None,
                            timeout: Optional[float] = None) -> Any:
        """Clear signal invert."""
        args: dict = {}
        if channels is not None:
            args["channels"] = channels
        return self._call_tool("clear_signal_invert", args, timeout=timeout)

    def get_signal_invert_config(self,
                                 timeout: Optional[float] = None) -> dict:
        """Get current signal invert configuration."""
        return self._call_tool("get_signal_invert_config", {},
                               timeout=timeout)

    # ---- 18. Repeat Status (1 tool) ----

    def get_repeat_status(self, timeout: Optional[float] = None) -> dict:
        """Get repeat/collect mode status."""
        return self._call_tool("get_repeat_status", {}, timeout=timeout)

    # ---- 19. Disk Cache (1 tool) ----

    def get_disk_cache_info(self, timeout: Optional[float] = None) -> dict:
        """Get disk cache info."""
        return self._call_tool("get_disk_cache_info", {}, timeout=timeout)

    # ---- 20. Batch B tools (10 tools) ----

    def refresh_device_list(self, timeout: Optional[float] = None) -> List[dict]:
        """Trigger a hot-plug rescan and return updated device list."""
        # refresh_device_list scans all drivers which can take a long time
        # on systems with many USB devices. Use a longer default timeout.
        return self._call_tool("refresh_device_list", {}, timeout=timeout or 120.0)

    def set_save_range(self, start_sample: int, end_sample: int,
                       timeout: Optional[float] = None) -> Any:
        """Set the save range in samples."""
        return self._call_tool("set_save_range", {
            "startSample": start_sample,
            "endSample": end_sample
        }, timeout=timeout)

    def reconfigure_decoder(self, analyzer_id: str,
                            options: Optional[dict] = None,
                            channel_map: Optional[dict] = None,
                            timeout: Optional[float] = None) -> Any:
        """Reconfigure an existing decoder in place."""
        args: dict = {"analyzerId": analyzer_id}
        if options is not None:
            args["options"] = options
        if channel_map is not None:
            args["channelMap"] = channel_map
        return self._call_tool("reconfigure_decoder", args, timeout=timeout)

    def get_decoder_class_names(self, analyzer_name: str,
                                timeout: Optional[float] = None) -> List[str]:
        """Get annotation class names for a decoder."""
        return self._call_tool("get_decoder_class_names", {
            "analyzerName": analyzer_name
        }, timeout=timeout)

    def get_decoder_binary_output(self, analyzer_id: str, output_id: int,
                                  timeout: Optional[float] = None) -> Any:
        """Read a decoder's binary output stream."""
        return self._call_tool("get_decoder_binary_output", {
            "analyzerId": analyzer_id,
            "outputId": output_id
        }, timeout=timeout)

    def get_math_results(self, timeout: Optional[float] = None) -> dict:
        """Read computed math trace results."""
        return self._call_tool("get_math_results", {}, timeout=timeout)

    def get_spectrum_results(self, timeout: Optional[float] = None) -> dict:
        """Read computed FFT spectrum results."""
        return self._call_tool("get_spectrum_results", {}, timeout=timeout)

    def get_lissajous_results(self, timeout: Optional[float] = None) -> dict:
        """Read Lissajous trace configuration."""
        return self._call_tool("get_lissajous_results", {}, timeout=timeout)

    def get_error_state(self, timeout: Optional[float] = None) -> dict:
        """Read the session error state."""
        return self._call_tool("get_error_state", {}, timeout=timeout)

    def clear_error_state(self, timeout: Optional[float] = None) -> Any:
        """Clear the session error state."""
        return self._call_tool("clear_error_state", {}, timeout=timeout)

    # ---- Utility methods ----

    def get_demo_device(self) -> dict:
        """Find and return the demo device from get_devices()."""
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

    def safe_capture_and_wait(self, device_id: str,
                              logic_config: Optional[dict] = None,
                              capture_config: Optional[dict] = None,
                              wait_timeout: float = 60.0) -> Any:
        """Convenience: start_capture + wait_capture with cleanup on error."""
        try:
            self.start_capture(device_id, logic_config, capture_config)
            return self.wait_capture(timeout_seconds=wait_timeout,
                                     timeout=wait_timeout + 10)
        except Exception:
            try:
                self.stop_capture()
            except Exception:
                pass
            raise
