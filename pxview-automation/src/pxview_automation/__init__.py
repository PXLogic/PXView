"""pxview-automation: Python automation client and CLI for PXView.

A pip-installable package that wraps all 61 MCP tools exposed by
PXView's JSON-RPC 2.0 over HTTP API, plus a high-level API and
a command-line tool (``pxview-cli``).

Quick start::

    pip install pxview-automation

    # Start PXView in headless mode
    PXView.exe --headless &

    # Use the Python API
    from pxview_automation import PXView
    with PXView() as pxv:
        pxv.connect()
        devices = pxv.list_devices()
        print(devices)

    # Or use the CLI
    # pxview-cli list-devices
    # pxview-cli capture --device demo --channels 0,1 --rate 1M --time 1s
"""

from __future__ import annotations

__version__ = "1.5.5"

from .exceptions import (
    ConfigError,
    McpConnectionError,
    McpError,
    ProcessError,
    PxvError,
)
from .client import McpClient
from .highlevel import PXView
from .process import PXViewProcess

__all__ = [
    # Core client
    "McpClient",
    # High-level API
    "PXView",
    # Process management
    "PXViewProcess",
    # Exceptions
    "PxvError",
    "McpError",
    "McpConnectionError",
    "ProcessError",
    "ConfigError",
    # Version
    "__version__",
]
