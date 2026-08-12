"""Pytest configuration for pxview-automation tests.

Registers the ``integration`` marker for tests that require a running
PXView headless server.
"""

import pytest


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "integration: tests that require a running PXView --headless server",
    )


def pytest_collection_modifyitems(config, items):
    """Auto-skip integration tests if the PXView server is not reachable."""
    import json
    import urllib.request

    # Quick connectivity check
    server_reachable = False
    try:
        raw = json.dumps(
            {"jsonrpc": "2.0", "id": 0, "method": "ping"}
        ).encode("utf-8")
        req = urllib.request.Request(
            "http://127.0.0.1:10110/mcp",
            data=raw,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            if resp.read().strip():
                server_reachable = True
    except Exception:
        pass

    if not server_reachable:
        skip_integration = pytest.mark.skip(
            reason="PXView server not reachable (start with: PXView.exe --headless)"
        )
        for item in items:
            if "integration" in item.keywords:
                item.add_marker(skip_integration)
