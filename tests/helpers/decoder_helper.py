"""
decoder_helper.py - Helper functions for decoder operations.
"""

from __future__ import annotations

import time
from typing import Optional

from pxview_automation import McpClient, McpError


def add_decoder_safe(mcp: McpClient, decoder_name: str,
                     channel_map: Optional[dict] = None,
                     options: Optional[dict] = None,
                     device_id: Optional[str] = None,
                     stack_on: Optional[str] = None,
                     timeout: float = 120.0) -> str:
    """Add a decoder and return its analyzer_id. Wraps settings dict."""
    settings: dict = {}
    if channel_map:
        settings["channelMap"] = channel_map
    if options:
        settings["options"] = options
    result = mcp.add_analyzer(
        analyzer_name=decoder_name,
        settings=settings if settings else None,
        device_id=device_id,
        stack_on_analyzer_id=stack_on,
        timeout=timeout,
    )
    # Parse the analyzer_id from various response formats
    if isinstance(result, (int, float)):
        return str(int(result))
    if isinstance(result, dict):
        aid = result.get("instance_id") or result.get("id") or result.get("analyzerId")
        if aid:
            return str(aid)
    if isinstance(result, str):
        return result
    raise McpError(f"Cannot parse analyzer_id from result: {result}")


def extract_annotations(result) -> list:
    """Unwrap the get_analyzer_results dict contract into an annotation list.

    Server contract (mcp_tool_registry.cpp handle_get_analyzer_results):
        {"annotations": [ann, ...]}
    where each ann is {"start_sample", "end_sample", "ann_class", "texts"}
    (see mcp_serializers.cpp decoder_ann_to_json).
    """
    if isinstance(result, dict):
        anns = result.get("annotations")
        return anns if isinstance(anns, list) else []
    return result or []


def get_decoder_results_with_retry(mcp: McpClient, analyzer_id: str,
                                   max_count: int = 10000,
                                   max_wait: float = 30.0,
                                   poll_interval: float = 1.0) -> list:
    """Get decoder results, retrying until data appears or timeout."""
    deadline = time.time() + max_wait
    last_result: list = []
    while time.time() < deadline:
        try:
            results = extract_annotations(
                mcp.get_analyzer_results(analyzer_id, max_count=max_count))
            if results:
                return results
            last_result = results
        except Exception:
            pass
        time.sleep(poll_interval)
    return last_result


def get_analyzer_id_by_name(mcp: McpClient, name: str) -> Optional[str]:
    """Find an active analyzer by decoder name and return its instance id."""
    decoders = mcp.get_active_decoders()
    for d in decoders:
        if d.get("decoder_id") == name or d.get("name") == name:
            return d.get("instance_id") or d.get("id")
    return None


KEY_DECODERS = [
    "i2c_c", "spi_c", "uart_c", "pwm_c", "can_c",
    "jtag_c", "swd_c", "onewire_c", "i2s_c",
    "modbus_c", "counter_c", "timing_c",
]
