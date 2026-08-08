"""Exception hierarchy for pxview-automation.

All exceptions derive from :class:`PxvError` so callers can catch
any package-level error with a single ``except PxvError``.

Hierarchy::

    PxvError
    ├── McpError              — MCP tool call returned an error
    │   └── McpConnectionError — cannot reach the MCP server
    ├── ProcessError          — PXView process management error
    └── ConfigError           — invalid configuration / arguments
"""

from __future__ import annotations

from typing import Any, Optional


class PxvError(Exception):
    """Base exception for all pxview-automation errors.

    Catch this if you want to handle any package-level error.
    """

    def __init__(self, message: str, code: int = -1, raw: Any = None):
        super().__init__(message)
        self.message = message
        self.code = code
        self.raw = raw

    def __repr__(self) -> str:
        return f"{self.__class__.__name__}(code={self.code}, message={self.message!r})"


class McpError(PxvError):
    """Raised when an MCP tool call returns an error response.

    Attributes:
        message: Human-readable error message.
        code:    JSON-RPC error code (negative for transport errors,
                 positive for application-level error codes).
        raw:     The full error response dict, if available.
    """


class McpConnectionError(McpError):
    """Raised when the MCP server cannot be reached or responds
    with an empty/invalid response.

    This typically means PXView is not running, or the ``--headless``
    process has not started the MCP transport on port 10110 yet.
    """


class ProcessError(PxvError):
    """Raised when PXView process management fails.

    For example: the executable was not found, the process exited
    with a non-zero code, or the MCP port never became reachable.
    """


class ConfigError(PxvError):
    """Raised when the caller provides invalid configuration or
    arguments that cannot be mapped to a valid MCP tool call."""
