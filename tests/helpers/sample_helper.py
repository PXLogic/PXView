"""sample_helper.py - logic sample payload decoding helpers.

``get_samples(channelType='logic')`` returns a **bit-packed bitmap**
(8 samples per byte, LSB-first — the response's ``bit_order='lsb0'``), the
same encoding as ``export_raw_data('binary')`` and the ``.pxl/.pxc`` files.
The response is self-describing:

    first_sample    absolute sample index of bit 0 of data[0]
                    (= requested start rounded DOWN to a byte boundary)
    sample_count    samples the payload covers
    byte_count      len(data) == ceil(sample_count / 8)
    truncated       True = clipped at a snapshot block boundary, continue
                    from first_sample + sample_count

This module is the single place that knows the layout.  Do not re-derive it
per test: the pre-1.6.5 tests each carried their own ``len(raw) // 8``
compensation for the old "declare N bytes, only N/8 carry real data" bug, and
that duplication is what made the payload-format regression hard to see.
"""

from __future__ import annotations

from typing import Optional


def unpack_logic_bits(
    raw: bytes,
    count: Optional[int] = None,
    *,
    first_sample: int = 0,
    start_sample: int = 0,
) -> list:
    """Expand a packed logic bitmap into sample levels (0/1).

    Args:
        raw:          The bytes returned by ``get_samples`` for a logic
                      channel (``first_sample`` is passed separately).
        count:        Number of samples to return.  None = everything from
                      ``start_sample`` to the end of ``raw``.
        first_sample: The response's ``first_sample`` field (byte-aligned
                      start of the payload).  Defaults to 0.
        start_sample: Absolute index of the first sample you want back.
                      Defaults to ``first_sample`` (i.e. start at bit 0).

    Returns:
        ``count`` levels starting at absolute sample ``start_sample``.
    """
    offset = max(0, start_sample - first_sample)
    bits = []
    for byte in raw:
        for k in range(8):
            bits.append((byte >> k) & 1)
    if offset:
        bits = bits[offset:]
    if count is not None:
        bits = bits[:count]
    return bits


def read_logic(mcp, channel: int, start: int = 0, end: Optional[int] = None):
    """Read one logic channel over MCP.

    Returns ``(packed_bytes, metadata)``.  Always take the covered sample
    count from ``metadata['sample_count']`` — never from ``len(bytes)``
    (the payload is a bitmap, so ``len == ceil(sample_count / 8)``).
    """
    import base64

    meta = mcp.get_samples_meta(
        channel_index=channel, channel_type="logic",
        start_sample=start, end_sample=end,
    )
    raw = meta.get("data", b"")
    if isinstance(raw, str):
        raw = base64.b64decode(raw)
    return (bytes(raw) if raw is not None else b""), meta


def logic_bit(raw: bytes, sample_index: int, *, first_sample: int = 0) -> int:
    """Level (0/1) of one absolute sample inside a packed payload."""
    rel = sample_index - first_sample
    if rel < 0 or (rel >> 3) >= len(raw):
        raise IndexError(
            f"sample {sample_index} outside payload "
            f"(first_sample={first_sample}, bytes={len(raw)})"
        )
    return (raw[rel >> 3] >> (rel & 7)) & 1
