#!/usr/bin/env python3
"""Assert that an X11 window has actually rendered content.

A WebKitGTK application can open a perfectly valid, correctly sized window that
stays completely blank, because its WebProcess died during startup. Every other
health signal still looks fine in that state:

  * the UI process is alive
  * the window exists and has the expected title and geometry
  * anything the app spawned keeps working -- for the PXView Agent that means
    the headless PXView child keeps serving MCP port 10110

So a health check based on processes and ports passes while the user stares at
a white rectangle. This check grabs the window with xwd and looks at pixels.

Exit codes:
  0  window found and rendered
  1  window not found
  2  window found but blank (uniform colour)

Usage:
  check_window_render.py "<window title substring>" [min-distinct-colours]

Requires x11-utils (xwininfo, xwd) and a reachable $DISPLAY.
"""
from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from collections import Counter

DISPLAY = os.environ.get("DISPLAY", ":0")
POLL_INTERVAL = 2.0
WINDOW_TIMEOUT = 90.0


def find_window(title: str, timeout: float = WINDOW_TIMEOUT) -> str | None:
    """Return the X window id whose tree entry contains `title`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            out = subprocess.run(
                ["xwininfo", "-root", "-tree", "-display", DISPLAY],
                capture_output=True, text=True, timeout=20,
            ).stdout
        except (OSError, subprocess.SubprocessError):
            out = ""
        for line in out.splitlines():
            if title.lower() in line.lower():
                parts = line.split()
                if parts and parts[0].startswith("0x"):
                    return parts[0]
        time.sleep(POLL_INTERVAL)
    return None


def capture(window_id: str, path: str) -> None:
    subprocess.run(
        ["xwd", "-id", window_id, "-display", DISPLAY, "-out", path],
        check=True, capture_output=True, timeout=120,
    )


def parse_xwd(path: str) -> tuple[int, int, int, bytes]:
    """Parse an XWD file into (width, height, bytes_per_line, raster).

    The 100-byte XWDFileHeader is 25 CARD32 written in the display's byte
    order, so try both endiannesses and keep the one giving sane geometry.
    """
    with open(path, "rb") as handle:
        data = handle.read()

    for endian in ("<", ">"):
        header = struct.unpack(endian + "25I", data[:100])
        width, height = header[4], header[5]
        if not (0 < width <= 16384 and 0 < height <= 16384):
            continue
        header_size, bytes_per_line, ncolors = header[0], header[11], header[18]
        offset = header_size + ncolors * 12
        raster = data[offset:offset + bytes_per_line * height]
        if len(raster) < bytes_per_line * height:
            continue
        return width, height, bytes_per_line, raster

    raise RuntimeError("could not parse XWD header")


def sample(width: int, height: int, bytes_per_line: int, raster: bytes,
           step: int = 4) -> tuple[Counter, int]:
    bpp = max(1, bytes_per_line // max(1, width))
    counts: Counter = Counter()
    total = 0
    for y in range(0, height, step):
        row = raster[y * bytes_per_line:(y + 1) * bytes_per_line]
        for x in range(0, width, step):
            i = x * bpp
            if i + 3 <= len(row):
                counts[row[i:i + 3]] += 1
                total += 1
    return counts, total


def main() -> int:
    for tool in ("xwininfo", "xwd"):
        if shutil.which(tool) is None:
            print(f"FAIL: {tool} not found (apt install x11-utils)", file=sys.stderr)
            return 1

    title = sys.argv[1] if len(sys.argv) > 1 else "PXView Agent"
    min_distinct = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    window = find_window(title)
    if window is None:
        print(f"FAIL: no window matching {title!r} on DISPLAY={DISPLAY} "
              f"within {WINDOW_TIMEOUT:.0f}s", file=sys.stderr)
        return 1
    print(f"window {window} ({title!r}) on DISPLAY={DISPLAY}")

    tmp = tempfile.NamedTemporaryFile(suffix=".xwd", delete=False)
    tmp.close()
    try:
        capture(window, tmp.name)
        width, height, bytes_per_line, raster = parse_xwd(tmp.name)
        counts, total = sample(width, height, bytes_per_line, raster)
    finally:
        os.unlink(tmp.name)

    if total == 0:
        print("FAIL: captured an empty image", file=sys.stderr)
        return 2

    distinct = len(counts)
    colour, hits = counts.most_common(1)[0]
    ratio = hits / total

    print(f"captured {width}x{height}: {total} samples, {distinct} distinct colours")
    print(f"dominant #{colour.hex()} covers {ratio:.1%}")

    if distinct < min_distinct:
        print(f"FAIL: only {distinct} distinct colours, need >= {min_distinct}; "
              "the window is blank", file=sys.stderr)
        return 2
    if ratio > 0.98:
        print(f"FAIL: a single colour covers {ratio:.1%} of the window; the "
              "WebProcess most likely crashed", file=sys.stderr)
        return 2

    print("PASS: window has real rendered content")
    return 0


if __name__ == "__main__":
    sys.exit(main())
