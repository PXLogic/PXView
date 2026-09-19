"""
test_13_signal_data.py - Signal data reading tests (Layer 7).

Validates get_logic_samples, get_analog_samples, get_dso_samples,
find_next_edge, find_pattern.
"""

import time

import pytest

from pxview_automation import McpClient, McpError
from helpers.assertions import assert_samples_non_empty
from helpers.capture_helper import do_timed_capture
from helpers.sample_helper import logic_bit, read_logic

pytestmark = pytest.mark.p1


class TestSignalData:

    def test_get_logic_samples_basic(self, mcp: McpClient, device_id: str,
                                     cleanup_after_test):
        """get_samples 返回位打包位图 + 自描述元数据。

        回归保护（V1.6.5）：不传 endSample 时必须**读到采集末尾**，而不是因为
        UINT64_MAX 哨兵在 `end - start + 1` 里回绕成 0 而返回空数据。
        """
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        raw, meta = read_logic(mcp, 0)
        assert len(raw) > 0, "默认读取（无 endSample）返回空数据"
        assert meta["sample_count"] > 0
        assert meta["first_sample"] == 0
        assert meta["bits_per_sample"] == 1
        assert meta["bit_order"] == "lsb0"
        assert meta["truncated"] is False
        # 自描述不变式：byte_count == len(data) == ceil(sample_count / 8)
        assert meta["byte_count"] == len(raw)
        assert meta["byte_count"] == (meta["sample_count"] + 7) // 8

    def test_get_logic_samples_range(self, mcp: McpClient, device_id: str,
                                     cleanup_after_test):
        """startSample/endSample 精确选中区间（含 end 端点）。"""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        raw, meta = read_logic(mcp, 0, start=0, end=100)
        assert len(raw) > 0
        assert meta["first_sample"] == 0
        assert meta["sample_count"] == 101, \
            f"请求 [0, 100] 应覆盖 101 个样本，实得 {meta['sample_count']}"
        assert meta["byte_count"] == (101 + 7) // 8

    def test_get_logic_samples_pagination(self, mcp: McpClient, device_id: str,
                                          cleanup_after_test):
        """按样本分页读，拼接结果与全量读逐字节一致。"""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        full, full_meta = read_logic(mcp, 0)
        total = full_meta["sample_count"]
        assert total > 0

        page = 8192  # 8 的整数倍 -> 每页首样本都落在字节边界
        rebuilt = bytearray()
        for start in range(0, min(total, page * 4), page):
            end = min(start + page, total) - 1
            chunk, meta = read_logic(mcp, 0, start=start, end=end)
            assert meta["first_sample"] == start, (
                f"page {start}: first_sample={meta['first_sample']} "
                f"（8 的整数倍起点不该发生字节对齐回退）")
            assert meta["byte_count"] == len(chunk)
            assert meta["byte_count"] == (meta["sample_count"] + 7) // 8
            rebuilt.extend(chunk)

        assert len(rebuilt) > 0
        assert bytes(rebuilt) == full[:len(rebuilt)], \
            "分页拼接结果与全量读不一致（位打包或对齐有偏移）"

    def test_get_logic_samples_end_negative_one(self, mcp: McpClient,
                                                device_id: str,
                                                cleanup_after_test):
        """endSample=-1 等价于"读到末尾"（UINT64_MAX 哨兵）。"""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)
        raw, meta = read_logic(mcp, 0, end=-1)
        assert len(raw) > 0, "endSample=-1 返回空数据"
        omitted_raw, omitted_meta = read_logic(mcp, 0)
        assert meta["sample_count"] == omitted_meta["sample_count"], \
            "-1 哨兵必须与省略 endSample 等价"
        assert meta["first_sample"] == omitted_meta["first_sample"]

    def test_get_logic_samples_bit_order_matches_edge(self, mcp: McpClient,
                                                      device_id: str,
                                                      cleanup_after_test):
        """位序 LSB-first：用 find_next_edge 作独立 oracle 交叉验证。

        packed 位图里 bit k 对应同一字节内偏移 k 的样本。位序若被写成
        MSB-first，边沿位置上的电平就会与 find_next_edge 的结论矛盾。
        """
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        raw, meta = read_logic(mcp, 0)
        first = meta["first_sample"]

        checked = 0
        for rising in (True, False):
            try:
                edge = mcp.find_next_edge(channel_index=0, from_sample=1,
                                          rising_edge=rising)
            except McpError:
                continue
            pos = edge["sample"] if isinstance(edge, dict) else int(edge)
            assert pos > 0
            assert logic_bit(raw, pos, first_sample=first) == (1 if rising else 0)
            assert logic_bit(raw, pos - 1, first_sample=first) == \
                (0 if rising else 1)
            checked += 1
        assert checked > 0, "采集中没有任何边沿，无法交叉验证位序"

    def test_get_logic_samples_byte_aligned_start(self, mcp: McpClient,
                                                  device_id: str,
                                                  cleanup_after_test):
        """非 8 对齐起点：first_sample 必须如实回显字节对齐后的实际起点。"""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)
        full, full_meta = read_logic(mcp, 0)

        raw13, meta13 = read_logic(mcp, 0, start=13, end=100)
        assert meta13["first_sample"] == 8, \
            f"start=13 应向下对齐到 8，实得 {meta13['first_sample']}"
        assert logic_bit(raw13, 13, first_sample=meta13["first_sample"]) == \
            logic_bit(full, 13, first_sample=full_meta["first_sample"]), \
            "绝对样本 13 的电平在窗口读与全量读之间不一致"

        raw5, meta5 = read_logic(mcp, 0, start=5, end=100)
        assert meta5["first_sample"] == 0
        assert logic_bit(raw5, 5, first_sample=0) == \
            logic_bit(full, 5, first_sample=full_meta["first_sample"])

    def test_find_next_edge(self, mcp: McpClient, device_id: str,
                            cleanup_after_test):
        """find_next_edge returns a valid sample index."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        result = mcp.find_next_edge(channel_index=0,
                                    from_sample=0,
                                    rising_edge=True)
        assert result is not None

    def test_find_next_edge_falling(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """find_next_edge with falling direction."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        result = mcp.find_next_edge(channel_index=0,
                                    from_sample=0,
                                    rising_edge=False)
        assert result is not None

    def test_find_pattern_exact(self, mcp: McpClient, device_id: str,
                                cleanup_after_test):
        """find_pattern with exact '0'/'1' pattern."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        result = mcp.find_pattern(from_sample=0,
                                  channel_index=0,
                                  pattern="1")
        assert result is not None

    def test_find_pattern_dont_care(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """find_pattern with 'x' wildcard."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        result = mcp.find_pattern(from_sample=0,
                                  channel_index=0,
                                  pattern="x")
        assert result is not None

    def test_find_pattern_multi_channel(self, mcp: McpClient, device_id: str,
                                         cleanup_after_test):
        """find_pattern with multi-channel combined search."""
        do_timed_capture(mcp, device_id, channels=[0, 1],
                         sample_rate=1000000, duration_seconds=0.5)
        result = mcp.find_pattern(
            from_sample=0,
            channels=[
                {"channelIndex": 0, "state": "1"},
                {"channelIndex": 1, "state": "0"},
            ])
        assert result is not None

    def test_get_logic_samples_no_capture(self, mcp: McpClient,
                                          cleanup_after_test):
        """get_logic_samples without capture returns error or empty."""
        try:
            result = mcp.get_samples(channel_type="logic", channel_index=0)
            # If it doesn't error, it should be empty/None
        except McpError:
            pass  # Expected

    def test_get_logic_samples_all_channels(self, mcp: McpClient, device_id: str,
                                            cleanup_after_test):
        """Read samples from all enabled channels."""
        channels = [0, 1, 2, 3]
        do_timed_capture(mcp, device_id, channels=channels,
                         sample_rate=1000000, duration_seconds=0.3)
        for ch in channels:
            samples = mcp.get_samples(channel_type="logic", channel_index=ch)
            assert len(samples) > 0, f"Channel {ch} has no samples"
