"""
export_helper.py - Helper functions for export/import operations.
"""

from __future__ import annotations

import csv
import os
import struct
from typing import List, Optional

from mcp_client import McpClient


def read_csv_file(filepath: str) -> List[dict]:
    """Read a CSV file and return rows as dicts."""
    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        reader = csv.DictReader(f)
        return list(reader)


def read_binary_file(filepath: str) -> bytes:
    """Read a binary file and return raw bytes."""
    with open(filepath, "rb") as f:
        return f.read()


def find_exported_csv(directory: str, pattern: str = "channel") -> List[str]:
    """Find CSV files matching a pattern in a directory."""
    result = []
    if not os.path.isdir(directory):
        return result
    for f in os.listdir(directory):
        if f.lower().endswith(".csv") and pattern.lower() in f.lower():
            result.append(os.path.join(directory, f))
    return result


def find_exported_binary(directory: str, pattern: str = "channel") -> List[str]:
    """Find binary files matching a pattern in a directory."""
    result = []
    if not os.path.isdir(directory):
        return result
    for f in os.listdir(directory):
        if (f.lower().endswith(".bin") or f.lower().endswith(".dat")
                or pattern.lower() in f.lower()):
            result.append(os.path.join(directory, f))
    return result


def get_logic_sample_bit(samples: bytes, sample_index: int) -> int:
    """Extract a single bit from logic sample bytes."""
    byte_idx = sample_index // 8
    bit_idx = sample_index % 8
    if byte_idx >= len(samples):
        return -1
    return (samples[byte_idx] >> bit_idx) & 1


def compare_logic_samples(data1: bytes, data2: bytes) -> bool:
    """Compare two logic sample byte arrays for equality."""
    return data1 == data2
