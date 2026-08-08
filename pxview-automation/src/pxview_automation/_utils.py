"""Internal utility functions for pxview-automation.

These are implementation details and not part of the public API.
"""

from __future__ import annotations

import os
import re
import sys


def to_windows_path(path: str) -> str:
    """Convert a MSYS2/Cygwin-style path to a Windows-native path.

    PXView is a native Windows application and cannot understand
    MSYS2 paths like ``/c/Users/...`` or ``/tmp/...``.
    This function converts such paths to ``C:\\Users\\...``
    or the equivalent Windows form.

    On non-Windows platforms the path is returned unchanged.
    """
    if not path or sys.platform != "win32":
        return path

    # Convert /c/Users/... → C:/Users/...
    m = re.match(r"^/([a-zA-Z])/(.*)$", path)
    if m:
        return f"{m.group(1).upper()}:/{m.group(2)}"

    # Convert /tmp/... → use TEMP env var
    if path.startswith("/tmp/"):
        tmp = os.environ.get("TEMP", os.environ.get("TMP", "C:\\Temp"))
        rest = path[5:]  # skip '/tmp/'
        return tmp.replace("\\\\", "/") + "/" + rest

    return path


def format_duration(seconds: float) -> str:
    """Format a duration in seconds as a human-readable string.

    Examples:
        0.5    → "0.5s"
        65.0   → "1m05s"
        3700.0 → "1h01m40s"
    """
    if seconds < 60:
        return f"{seconds:.1f}s"
    if seconds < 3600:
        m = int(seconds // 60)
        s = seconds % 60
        return f"{m}m{s:04.1f}s"
    h = int(seconds // 3600)
    m = int((seconds % 3600) // 60)
    s = seconds % 60
    return f"{h}h{m:02d}m{s:04.1f}s"


def parse_duration(s: str) -> float:
    """Parse a human-readable duration string into seconds.

    Supported formats:
        "100ms"  → 0.1
        "2s"     → 2.0
        "1.5s"   → 1.5
        "3m"     → 180.0
        "1h"     → 3600.0
        "90"     → 90.0  (bare number = seconds)

    Raises:
        ValueError: if the string cannot be parsed.
    """
    s = s.strip().lower()
    if not s:
        raise ValueError("Empty duration string")

    # Bare number → seconds
    try:
        return float(s)
    except ValueError:
        pass

    units = {
        "ms": 0.001,
        "s": 1.0,
        "m": 60.0,
        "min": 60.0,
        "h": 3600.0,
        "hr": 3600.0,
    }

    for unit, factor in units.items():
        if s.endswith(unit):
            num_str = s[: -len(unit)].strip()
            try:
                return float(num_str) * factor
            except ValueError:
                raise ValueError(f"Invalid duration: {s!r}")

    raise ValueError(f"Unknown duration unit in: {s!r}")


def parse_int_list(s: str) -> list[int]:
    """Parse a comma-separated list of integers.

    Examples:
        "0,1,2"    → [0, 1, 2]
        "0-3"      → [0, 1, 2, 3]
        "0,2-4,6"  → [0, 2, 3, 4, 6]

    Raises:
        ValueError: if the string cannot be parsed.
    """
    result: list[int] = []
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            result.extend(range(int(lo), int(hi) + 1))
        else:
            result.append(int(part))
    return result
