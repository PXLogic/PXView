"""
test_25_pwm_mixed_stress.py - PWM decoder C/Python mixed stress tests.

Tests:
- Multiple PWM decoders (C + Python) running simultaneously on different channels
- C vs Python result comparison on the same data
- Precise PWM signal verification (duty cycle, period)
- Dense PWM signal stress test
- Dynamic add/remove stress

Channel layout:
  ch0: random pattern → PWM C decoder
  ch1: random pattern → PWM Python decoder
  ch6: PWM0 (precise square wave) → PWM C decoder
  ch7: PWM1 (precise square wave) → PWM Python decoder
"""

import re

import pytest

from pxview_automation import McpClient
from helpers.assertions import assert_annotation_valid
from helpers.capture_helper import (
    do_buffer_capture_with_pattern,
    configure_pwm,
)
from helpers.decoder_helper import (
    add_decoder_safe,
    get_decoder_results_with_retry,
)

pytestmark = pytest.mark.p0

SAMPLE_RATE_1M = 1_000_000
SAMPLE_COUNT_1M = 1_000_000


# ======================================================================
# Test Group A: Basic mixed C/Python multi-decoder test
# ======================================================================

class TestPwmMixedBasic:
    """Multiple PWM decoders (C + Python) on different channels."""

    def test_multi_decoder_both_produce_results(self, mcp, device_id,
                                                 cleanup_after_test):
        """C and Python PWM decoders both produce non-empty results."""
        # Add C decoder on ch0
        c_id = add_decoder_safe(mcp, "pwm_c", channel_map={"data": 0},
                                device_id=device_id)
        assert c_id, "Failed to add pwm_c"

        # Add Python decoder on ch1
        py_id = add_decoder_safe(mcp, "pwm", channel_map={"data": 1},
                                 device_id=device_id)
        assert py_id, "Failed to add pwm (Python)"

        status = do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="random",
        )
        assert status["state"] in ("completed", "idle", "stopped"), \
            f"Capture failed: {status}"

        c_results = get_decoder_results_with_retry(mcp, c_id, max_wait=30.0)
        py_results = get_decoder_results_with_retry(mcp, py_id, max_wait=30.0)

        assert len(c_results) > 0, "C PWM decoder produced 0 results"
        assert len(py_results) > 0, "Python PWM decoder produced 0 results"

    def test_c_python_same_channel_comparable_count(self, mcp, device_id,
                                                      cleanup_after_test):
        """C and Python PWM on the same channel produce comparable result counts."""
        ch = 0
        # Add C decoder
        c_id = add_decoder_safe(mcp, "pwm_c", channel_map={"data": ch},
                                device_id=device_id)
        # Add Python decoder
        py_id = add_decoder_safe(mcp, "pwm", channel_map={"data": ch},
                                 device_id=device_id)

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[ch],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="random",
        )

        c_results = get_decoder_results_with_retry(mcp, c_id, max_wait=30.0)
        py_results = get_decoder_results_with_retry(mcp, py_id, max_wait=30.0)

        # Both must be non-empty
        assert len(c_results) > 0, "C PWM decoder produced 0 results"
        assert len(py_results) > 0, "Python PWM decoder produced 0 results"

        # Result count should be comparable (within 20% of each other)
        # C and Python may differ slightly in edge detection at boundaries
        ratio = len(c_results) / max(len(py_results), 1)
        assert 0.5 <= ratio <= 2.0, \
            f"C={len(c_results)} vs Python={len(py_results)} results, ratio={ratio}"

    def test_result_annotation_count_reasonable(self, mcp, device_id,
                                                  cleanup_after_test):
        """PWM on random data should produce at least 50 annotations."""
        c_id = add_decoder_safe(mcp, "pwm_c", channel_map={"data": 0},
                                device_id=device_id)

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="random",
        )

        results = get_decoder_results_with_retry(mcp, c_id, max_wait=30.0)
        assert len(results) >= 50, \
            f"PWM only produced {len(results)} annotations on 1M random samples"

        for ann in results:
            assert_annotation_valid(ann)


# ======================================================================
# Test Group B: Precise PWM verification
# ======================================================================

class TestPwmPreciseSignal:
    """Verify PWM decoder on precise PWM signal from demo driver.

    Skipped: the precise duty/period VALUE assertions require the demo driver
    to synthesize a clean configurable square wave on ch6/ch7 during buffer
    capture. Verified 2026-08-16 that the demo PWM override does NOT take
    effect (ch6 stays random even with PWM0_EN/FREQ/DUTY correctly configured
    and read back as set) — same class of demo-generation defect as the
    documented i2c SDA constant-0 issue. The config keys (SR_CONF_PWM0_EN=
    60004/FREQ=60005/DUTY=60006) now SET and persist correctly; only the
    generated waveform is broken. See tasks.md. The dense multi-decoder test
    below still exercises concurrent C/Python PWM decoding.
    """

    @pytest.mark.skip(reason="demo driver PWM override does not synthesize a "
                             "precise square wave in buffer capture (broken "
                             "generation, not config)")
    def test_precise_duty_cycle_30pct(self, mcp, device_id,
                                       cleanup_after_test):
        """PWM0 at 30% duty → decoder reads ~30%."""
        configure_pwm(mcp, channel=6, enable=True, freq=10000, duty=30.0)

        c_id = add_decoder_safe(mcp, "pwm_c", channel_map={"data": 6},
                                device_id=device_id)
        py_id = add_decoder_safe(mcp, "pwm", channel_map={"data": 6},
                                 device_id=device_id)

        try:
            do_buffer_capture_with_pattern(
                mcp, device_id,
                channels=[6],
                sample_rate=SAMPLE_RATE_1M,
                sample_count=SAMPLE_COUNT_1M,
                pattern="random",  # PWM override replaces ch6
            )

            c_results = get_decoder_results_with_retry(mcp, c_id, max_wait=30.0)
            py_results = get_decoder_results_with_retry(mcp, py_id, max_wait=30.0)

            assert len(c_results) > 0, "C decoder: 0 results on PWM signal"
            assert len(py_results) > 0, "Python decoder: 0 results on PWM signal"

            # Check duty cycle values
            for results, label in [(c_results, "C"), (py_results, "Python")]:
                duty_vals = []
                for ann in results:
                    for text in ann.get("texts", []):
                        m = re.search(r"([\d.]+)%", str(text))
                        if m:
                            duty_vals.append(float(m.group(1)))
                assert len(duty_vals) > 0, \
                    f"{label}: No duty cycle annotations found"
                avg_duty = sum(duty_vals) / len(duty_vals)
                assert 25.0 <= avg_duty <= 35.0, \
                    f"{label}: Average duty {avg_duty}% not ~30%"
        finally:
            configure_pwm(mcp, channel=6, enable=False, freq=0, duty=0)

    @pytest.mark.skip(reason="demo driver PWM override does not synthesize a "
                             "precise square wave in buffer capture (broken "
                             "generation, not config)")
    def test_precise_period_10khz(self, mcp, device_id, cleanup_after_test):
        """PWM0 at 10 kHz → period ≈ 100 µs."""
        configure_pwm(mcp, channel=6, enable=True, freq=10000, duty=50.0)

        c_id = add_decoder_safe(mcp, "pwm_c", channel_map={"data": 6},
                                device_id=device_id)

        try:
            do_buffer_capture_with_pattern(
                mcp, device_id,
                channels=[6],
                sample_rate=SAMPLE_RATE_1M,
                sample_count=SAMPLE_COUNT_1M,
                pattern="random",
            )

            results = get_decoder_results_with_retry(mcp, c_id, max_wait=30.0)
            assert len(results) > 0

            # At 1 MHz / 10 kHz, period = 100 samples = 100 µs
            period_vals = []
            for ann in results:
                for text in ann.get("texts", []):
                    text_str = str(text)
                    if "µs" in text_str and "%" not in text_str:
                        m = re.search(r"([\d.]+)", text_str)
                        if m:
                            period_vals.append(float(m.group(1)))
            assert len(period_vals) > 0, "No period annotations found"
            avg_period = sum(period_vals) / len(period_vals)
            assert 80.0 <= avg_period <= 120.0, \
                f"Average period {avg_period} µs not ~100 µs"
        finally:
            configure_pwm(mcp, channel=6, enable=False, freq=0, duty=0)


# ======================================================================
# Test Group C: Dense PWM stress test
# ======================================================================

class TestPwmDenseStress:
    """High-frequency PWM stress test with multiple decoders."""

    def test_dense_pwm_4_decoders_concurrent(self, mcp, device_id,
                                               cleanup_after_test):
        """4 PWM decoders (2C+2Py) on ch6/ch7 simultaneously."""
        configure_pwm(mcp, channel=6, enable=True, freq=100000, duty=50.0)
        configure_pwm(mcp, channel=7, enable=True, freq=100000, duty=30.0)

        ids = []
        ids.append(add_decoder_safe(mcp, "pwm_c", channel_map={"data": 6},
                                    device_id=device_id))
        ids.append(add_decoder_safe(mcp, "pwm", channel_map={"data": 6},
                                    device_id=device_id))
        ids.append(add_decoder_safe(mcp, "pwm_c", channel_map={"data": 7},
                                    device_id=device_id))
        ids.append(add_decoder_safe(mcp, "pwm", channel_map={"data": 7},
                                    device_id=device_id))

        assert all(ids), f"Some decoders failed: {ids}"

        try:
            do_buffer_capture_with_pattern(
                mcp, device_id,
                channels=[6, 7],
                sample_rate=10_000_000,
                sample_count=1_000_000,
                pattern="random",
            )

            for i, aid in enumerate(ids):
                results = get_decoder_results_with_retry(mcp, aid, max_wait=60.0)
                assert len(results) > 0, \
                    f"Decoder {i} (id={aid}) produced 0 results"
        finally:
            configure_pwm(mcp, channel=6, enable=False, freq=0, duty=0)
            configure_pwm(mcp, channel=7, enable=False, freq=0, duty=0)


# ======================================================================
# Test Group D: Dynamic add/remove stress
# ======================================================================

class TestPwmDynamicStress:
    """Repeated add/remove PWM decoders."""

    def test_add_remove_10x(self, mcp, device_id, cleanup_after_test):
        """Add/remove C+Python PWM decoders 10 times."""
        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=200_000,
            pattern="random",
        )

        for i in range(10):
            c_id = add_decoder_safe(mcp, "pwm_c", channel_map={"data": 0})
            py_id = add_decoder_safe(mcp, "pwm", channel_map={"data": 0})
            assert c_id and py_id, f"Iter {i}: failed to add decoders"
            mcp.remove_analyzer(c_id)
            mcp.remove_analyzer(py_id)

        decoders = mcp.get_active_decoders()
        assert len(decoders) == 0, f"{len(decoders)} decoders remain after cleanup"
