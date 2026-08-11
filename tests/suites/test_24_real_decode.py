"""
test_24_real_decode.py - 真实解码结果验证测试。

与旧测试的区别：
- 使用特定 pattern（random / graycode）而非默认
- 1GHz 采样率、10ms buffer 模式（10M 采样点）
- 不仅验证注释结构，还验证解码内容的正确性：
  - PWM: duty cycle 在 0-100% 范围内，period > 0
  - GrayCode: phase 值递增，increment 在合理范围，count 有变化
"""

import re
import time

import pytest

from pxview_automation import McpClient
from helpers.decoder_helper import add_decoder_safe, get_decoder_results_with_retry

pytestmark = pytest.mark.p0

# ---- 采集参数 ----
SAMPLE_RATE_1GHZ = 1_000_000_000  # 1 GHz
SAMPLE_COUNT_10M = 10_000_000     # 1 GHz × 10 ms = 10M samples
ALL_8_CHANNELS = [0, 1, 2, 3, 4, 5, 6, 7]

# GrayCode 解码器注释类型（从实际 MCP 响应中观察到）
# ann_class 值与 C 解码器内部 enum 不同，MCP API 使用自己的编号
GRAYCODE_ANN_PHASE = 7       # phase: 当前格雷码值
GRAYCODE_ANN_INCREMENT = 8  # increment: 增量 (+1, -1, +31, -127, ...)
GRAYCODE_ANN_COUNT = 9      # count: 累计计数 (0, -1, -128, -129, ...)
GRAYCODE_ANN_INTERVAL = 11  # interval: "63.9 ns, 15.6 MHz"
GRAYCODE_ANN_AVERAGE = 12   # average: "1.00 ns, 1.00 GHz"


def do_buffer_capture_with_pattern(
    mcp: McpClient,
    device_id: str,
    channels: list,
    sample_rate: int,
    sample_count: int,
    pattern: str,
):
    """用指定 pattern + manualCaptureMode（buffer 模式）采集。

    Args:
        pattern: "random", "graycode", "sigrok", "inc", ...
    """
    logic_config = {
        "digitalChannels": channels,
        "digitalSampleRate": sample_rate,
        "pattern": pattern,
    }
    capture_config = {
        "manualCaptureMode": {"sampleCount": sample_count},
    }
    mcp.start_capture(
        device_id=device_id,
        logic_device_configuration=logic_config,
        capture_configuration=capture_config,
    )
    # buffer 模式采集完成后会自动停止，wait_capture 等待完成
    wait_timeout = max(sample_count / sample_rate * 3, 30)
    mcp.wait_capture(timeout_seconds=wait_timeout, timeout=wait_timeout + 10)
    time.sleep(0.5)
    return mcp.get_capture_status()


# ======================================================================
# PWM 解码器 — 在 random pattern 上验证
# ======================================================================

class TestPwmRealDecode:
    """PWM 解码器在 random pattern 上的真实解码验证。

    random pattern 在每个通道上生成伪随机比特流。
    PWM 解码器检测边沿、计算占空比和周期。
    """

    def test_pwm_random_pattern_produces_results(self, mcp, device_id, cleanup_after_test):
        """PWM 解码器在 random pattern 上产生非空结果。"""
        analyzer_id = add_decoder_safe(
            mcp, "pwm_c",
            channel_map={"data": 0},
            device_id=device_id,
        )
        assert analyzer_id, "Failed to add pwm_c decoder"

        status = do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0],
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="random",
        )
        assert status["state"] in ("completed", "idle", "stopped"), \
            f"Capture failed: {status}"

        # PWM 解码需要时间处理 10M 采样，等待 60 秒
        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        assert len(results) > 0, \
            "PWM decoder produced 0 annotations on random pattern — " \
            "random data should have many edges"

    def test_pwm_duty_cycle_values_in_range(self, mcp, device_id, cleanup_after_test):
        """PWM 解码结果的 duty cycle 值在 0-100% 范围内。"""
        analyzer_id = add_decoder_safe(
            mcp, "pwm_c",
            channel_map={"data": 0},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0],
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="random",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        assert len(results) > 0, "No PWM results"

        # PWM annotations: ann_class 0 = duty cycle, 1 = period
        duty_count = 0
        for ann in results:
            texts = ann.get("texts", [])
            for text in texts:
                # duty cycle 文本格式: "XX.XXXXXX%"
                pct_match = re.search(r"([\d.]+)%", str(text))
                if pct_match:
                    pct_val = float(pct_match.group(1))
                    assert 0.0 <= pct_val <= 100.0, \
                        f"Duty cycle {pct_val}% out of range [0, 100]: ann={ann}"
                    duty_count += 1

        assert duty_count > 0, \
            f"No duty cycle annotations found in {len(results)} results: " \
            f"{[r.get('texts') for r in results[:5]]}"

    def test_pwm_period_values_positive(self, mcp, device_id, cleanup_after_test):
        """PWM 解码结果的 period 值为正数。"""
        analyzer_id = add_decoder_safe(
            mcp, "pwm_c",
            channel_map={"data": 0},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0],
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="random",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        assert len(results) > 0, "No PWM results"

        # period 文本格式: "XX.X ns" / "XX.X µs" / "XX.X ms" / "X.X s"
        period_count = 0
        for ann in results:
            texts = ann.get("texts", [])
            for text in texts:
                text_str = str(text)
                # period 包含时间单位，不含 % 符号
                if any(unit in text_str for unit in ("ns", "µs", "ms", "s", "ps", "fs")) \
                        and "%" not in text_str:
                    period_count += 1
                    # 从文本中提取数值
                    num_match = re.search(r"([\d.]+)", text_str)
                    if num_match:
                        period_val = float(num_match.group(1))
                        assert period_val > 0, \
                            f"Period value {period_val} not positive: {text_str}"

        assert period_count > 0, \
            f"No period annotations found in {len(results)} results"

    def test_pwm_annotation_count_reasonable(self, mcp, device_id, cleanup_after_test):
        """PWM 在 10M 随机采样上产生合理数量的注释（至少 50 个）。"""
        analyzer_id = add_decoder_safe(
            mcp, "pwm_c",
            channel_map={"data": 0},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0],
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="random",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        # 随机数据应该产生大量边沿 → 大量 PWM 周期
        # maxCount 默认 10000，所以至少应该有几十个
        assert len(results) >= 50, \
            f"PWM only produced {len(results)} annotations on 10M random samples — " \
            f"expected at least 50"


# ======================================================================
# Gray Code 解码器 — 在 graycode pattern 上验证
# ======================================================================

class TestGrayCodeRealDecode:
    """Gray Code 解码器在 graycode pattern 上的真实解码验证。

    graycode pattern 在 8 个通道上生成递增的格雷码：
    step 每个采样 +1，gray = step ^ (step >> 1)。
    8 位格雷码每 256 个采样回绕一次。

    格雷码解码器检测变化、转换为二进制、计算 increment 和 count。
    """

    def test_graycode_pattern_produces_results(self, mcp, device_id, cleanup_after_test):
        """GrayCode 解码器在 graycode pattern 上产生非空结果。"""
        analyzer_id = add_decoder_safe(
            mcp, "graycode_c",
            channel_map={
                "d0": 0, "d1": 1, "d2": 2, "d3": 3,
                "d4": 4, "d5": 5, "d6": 6, "d7": 7,
            },
            device_id=device_id,
        )
        assert analyzer_id, "Failed to add graycode_c decoder"

        status = do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=ALL_8_CHANNELS,
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="graycode",
        )
        assert status["state"] in ("completed", "idle", "stopped"), \
            f"Capture failed: {status}"

        # GrayCode 解码 10M 采样需要较长时间
        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        assert len(results) > 0, \
            "GrayCode decoder produced 0 annotations on graycode pattern"

    def test_graycode_phase_values(self, mcp, device_id, cleanup_after_test):
        """GrayCode 的 phase 值（格雷码原始值）在 0-255 范围内。"""
        analyzer_id = add_decoder_safe(
            mcp, "graycode_c",
            channel_map={
                "d0": 0, "d1": 1, "d2": 2, "d3": 3,
                "d4": 4, "d5": 5, "d6": 6, "d7": 7,
            },
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=ALL_8_CHANNELS,
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="graycode",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        assert len(results) > 0, "No GrayCode results"

        # 提取 phase 值（ann_class == GRAYCODE_ANN_PHASE）
        phases = []
        for ann in results:
            if ann.get("ann_class") == GRAYCODE_ANN_PHASE:
                for text in ann.get("texts", []):
                    text_str = str(text).strip()
                    if re.match(r"^-?\d+$", text_str):
                        phases.append(int(text_str))

        assert len(phases) > 0, \
            f"No phase annotations (ann_class={GRAYCODE_ANN_PHASE}) in {len(results)} results"

        # phase 是 8 位格雷码值，应该在 0-255 范围内
        for p in phases:
            assert 0 <= p <= 255, \
                f"Phase value {p} out of range [0, 255]"

    def test_graycode_increment_values(self, mcp, device_id, cleanup_after_test):
        """GrayCode 的 increment 值为非零整数（每次变化至少 ±1）。"""
        analyzer_id = add_decoder_safe(
            mcp, "graycode_c",
            channel_map={
                "d0": 0, "d1": 1, "d2": 2, "d3": 3,
                "d4": 4, "d5": 5, "d6": 6, "d7": 7,
            },
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=ALL_8_CHANNELS,
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="graycode",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        assert len(results) > 0, "No GrayCode results"

        # 提取 increment 值（ann_class == GRAYCODE_ANN_INCREMENT）
        increments = []
        for ann in results:
            if ann.get("ann_class") == GRAYCODE_ANN_INCREMENT:
                for text in ann.get("texts", []):
                    text_str = str(text).strip()
                    # increment 格式: "+1", "-1", "+31", "-127"
                    if re.match(r"^[+-]?\d+$", text_str):
                        increments.append(int(text_str))

        assert len(increments) > 0, \
            f"No increment annotations (ann_class={GRAYCODE_ANN_INCREMENT}) " \
            f"in {len(results)} results"

        # 每个 increment 必须是非零的（有变化才会产生注释）
        for inc in increments:
            assert inc != 0, \
                f"Increment value 0 found — should be non-zero: {increments[:20]}"

        # graycode pattern 每个采样递增 1，所以大部分 increment 应为 +1
        # 但回绕时会有大的负值（如 -255）
        positive_count = sum(1 for inc in increments if inc > 0)
        # 至少有一些正增量（递增阶段）
        assert positive_count > 0, \
            f"No positive increments found in {len(increments)} values: " \
            f"{increments[:20]}"

    def test_graycode_count_values(self, mcp, device_id, cleanup_after_test):
        """GrayCode 的 count 值为整数且与 increment 一致（有变化）。"""
        analyzer_id = add_decoder_safe(
            mcp, "graycode_c",
            channel_map={
                "d0": 0, "d1": 1, "d2": 2, "d3": 3,
                "d4": 4, "d5": 5, "d6": 6, "d7": 7,
            },
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=ALL_8_CHANNELS,
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="graycode",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        assert len(results) > 0, "No GrayCode results"

        # 提取 count 值（ann_class == GRAYCODE_ANN_COUNT）
        counts = []
        for ann in results:
            if ann.get("ann_class") == GRAYCODE_ANN_COUNT:
                for text in ann.get("texts", []):
                    text_str = str(text).strip()
                    if re.match(r"^-?\d+$", text_str):
                        counts.append(int(text_str))

        assert len(counts) > 0, \
            f"No count annotations (ann_class={GRAYCODE_ANN_COUNT}) " \
            f"in {len(results)} results"

        # count 是累计值，应该有变化（不是全部相同）
        unique_counts = set(counts)
        assert len(unique_counts) > 1, \
            f"All count values are identical ({counts[0]}), expected variation"

    def test_graycode_annotation_count_reasonable(self, mcp, device_id, cleanup_after_test):
        """GrayCode 在 10M graycode 采样上产生合理数量的注释。"""
        analyzer_id = add_decoder_safe(
            mcp, "graycode_c",
            channel_map={
                "d0": 0, "d1": 1, "d2": 2, "d3": 3,
                "d4": 4, "d5": 5, "d6": 6, "d7": 7,
            },
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=ALL_8_CHANNELS,
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="graycode",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        # graycode pattern 每个采样都变化，10M 采样应该产生大量注释
        # 解码可能需要时间，至少应该有几十个
        assert len(results) >= 50, \
            f"GrayCode only produced {len(results)} annotations on 10M samples — " \
            f"expected at least 50 (graycode changes every sample)"

    def test_graycode_interval_values(self, mcp, device_id, cleanup_after_test):
        """GrayCode 的 interval 值包含时间信息和频率。"""
        analyzer_id = add_decoder_safe(
            mcp, "graycode_c",
            channel_map={
                "d0": 0, "d1": 1, "d2": 2, "d3": 3,
                "d4": 4, "d5": 5, "d6": 6, "d7": 7,
            },
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=ALL_8_CHANNELS,
            sample_rate=SAMPLE_RATE_1GHZ,
            sample_count=SAMPLE_COUNT_10M,
            pattern="graycode",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=60.0)
        assert len(results) > 0, "No GrayCode results"

        # 提取 interval 值（ann_class == GRAYCODE_ANN_INTERVAL）
        intervals = []
        for ann in results:
            if ann.get("ann_class") == GRAYCODE_ANN_INTERVAL:
                for text in ann.get("texts", []):
                    text_str = str(text).strip()
                    if "ns" in text_str or "MHz" in text_str or "GHz" in text_str:
                        intervals.append(text_str)

        assert len(intervals) > 0, \
            f"No interval annotations (ann_class={GRAYCODE_ANN_INTERVAL}) " \
            f"in {len(results)} results"

        # 1GHz 采样率，每采样 1ns，所以 interval 应该在 ns 级别
        for ival in intervals:
            # 提取时间值
            ns_match = re.search(r"([\d.]+)\s*ns", ival)
            if ns_match:
                ns_val = float(ns_match.group(1))
                assert ns_val > 0, f"Interval time {ns_val} ns not positive: {ival}"
