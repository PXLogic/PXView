/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "measurecalculator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../data/dsosnapshot.h"
#include "../data/sessiondata.h"
#include "../data/signalmodel.h"
#include "../dsvdef.h"
#include "../log.h"

namespace pv {
namespace core {

// ---------------------------------------------------------------------------
// compute() — read DsoSnapshot + SignalModel, produce raw MeasurementResult
// ---------------------------------------------------------------------------

std::vector<MeasureCalculator::MeasurementResult>
MeasureCalculator::compute(SessionData *data,
                           const std::vector<std::shared_ptr<data::SignalModel>> &signal_models,
                           int channel_index,
                           int view_rect_height)
{
    std::vector<MeasurementResult> results;

    if (!data) {
        pxv_warn("%s", "MeasureCalculator::compute: SessionData is null");
        return results;
    }

    auto *dso = data->get_dso();
    if (!dso || dso->empty()) {
        return results;
    }

    const uint64_t sample_count = dso->get_sample_count();
    if (sample_count == 0) {
        return results;
    }

    // Resolve view_rect_height default (headless mode). The original View
    // code used DsoSignal::get_view_rect().height() which is the pixel
    // height of the DSO trace. In headless we fall back to the standard
    // DS_CONF_DSO_VDIVS * DefaultPixelsPerDiv so the voltage conversion
    // formula stays dimensionally consistent.
    if (view_rect_height <= 0) {
        view_rect_height = DefaultViewRectHeight;
    }

    // Build the list of DSO channels to process. The original DsoMeasure
    // was per-DsoSignal, so each DSO channel gets its own MeasurementResult.
    // We iterate signal_models to find DSO channels (type == SR_CHANNEL_DSO).
    std::vector<std::shared_ptr<data::SignalModel>> dso_models;
    for (const auto &m : signal_models) {
        if (!m) continue;
        if (m->type() != SR_CHANNEL_DSO) continue;
        if (channel_index != -1 && m->index() != channel_index) continue;
        if (!dso->has_data(m->index())) continue;
        dso_models.push_back(m);
    }

    for (const auto &m : dso_models) {
        MeasurementResult r;
        r.channel_index = m->index();
        r.hw_offset = (int)m->hw_offset();

        // get_samples returns a pointer to the contiguous sample buffer
        // for this channel (signal index → order conversion is internal).
        // start_sample=0, end_sample=sample_count-1 returns the whole
        // channel buffer. The lock is held only during get_samples; the
        // returned pointer is valid as long as the snapshot is not
        // modified concurrently (stopped captures are stable).
        const uint8_t *samples = nullptr;
        if (sample_count >= 1) {
            samples = dso->get_samples(
                0, (int64_t)(sample_count - 1), (uint16_t)m->index());
        }

        if (!samples) {
            pxv_err("MeasureCalculator::compute: get_samples returned null for channel %d",
                    m->index());
            results.push_back(r);
            continue;
        }

        // ---- max / min (equivalent to DsoSnapshot::get_max_min_value) ----
        uint8_t maxv = samples[0];
        uint8_t minv = samples[0];
        for (uint64_t i = 1; i < sample_count; i++) {
            const uint8_t v = samples[i];
            if (v > maxv) maxv = v;
            if (v < minv) minv = v;
        }
        r.max = maxv;
        r.min = minv;

        // ---- rms (equivalent to DsoSnapshot::cal_vrms(zero_off, order)) ----
        // cal_vrms computes sqrt(sum((zero_off - sample)^2) / count).
        // We replicate the formula without the VrmsScaleFactor chunking
        // (which is just for numerical stability with large counts — the
        // mathematical result is identical).
        {
            double sum_sq = 0.0;
            const double zero_off = (double)r.hw_offset;
            for (uint64_t i = 0; i < sample_count; i++) {
                const double diff = zero_off - (double)samples[i];
                sum_sq += diff * diff;
            }
            r.rms = std::sqrt(sum_sq / (double)sample_count);
        }

        // ---- mean (equivalent to DsoSnapshot::cal_vmean(order)) ----
        // cal_vmean computes sum(samples) / count.
        {
            double sum = 0.0;
            for (uint64_t i = 0; i < sample_count; i++) {
                sum += (double)samples[i];
            }
            r.mean = sum / (double)sample_count;
        }

        r.mValid = true;

        // ---- level-dependent measurements (high/low/period/rise/fall/...
        // Now fully implemented via compute_level_measurements(). This
        // replaces the original "never computed" behavior where level_valid
        // was always false and all level-dependent measurements returned "--".
        const double samplerate = dso->samplerate();
        if (samplerate > 0.0 && sample_count >= 4) {
            compute_level_measurements(samples, sample_count,
                                       samplerate, r);
        }

        results.push_back(r);
    }

    return results;
}

// ---------------------------------------------------------------------------
// compute_level_measurements() — level-dependent measurement computation
//
// Implements high/low level detection, period/frequency, rise/fall time,
// pulse width, duty cycle, burst time, and pulse count from the raw DSO
// sample buffer. Uses a histogram-based approach for high/low level
// detection and a mid-threshold crossing method for timing measurements.
//
// DSO ADC convention (inverted): ADC value 0 = max voltage, ADC value
// 255 = min voltage. In this codebase's naming:
//   - `high` = larger ADC value = low-voltage steady-state
//   - `low`  = smaller ADC value = high-voltage steady-state
// (this matches the original to_measurement_values formulas where
//  VHIG = hw_offset - low, VLOW = hw_offset - high).
// ---------------------------------------------------------------------------

void MeasureCalculator::compute_level_measurements(const uint8_t *samples,
                                                   uint64_t sample_count,
                                                   double samplerate,
                                                   MeasurementResult &r)
{
    // Time per sample in nanoseconds.
    const double ns_per_sample = 1.0e9 / samplerate;

    // -- Step 1: High/Low level detection via histogram --
    // Build a 256-bin histogram of ADC values and find the two most
    // prominent peaks. The peak at smaller ADC values = high-voltage
    // steady-state (= `low` field), the peak at larger ADC values =
    // low-voltage steady-state (= `high` field).
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (uint64_t i = 0; i < sample_count; i++) {
        hist[samples[i]]++;
    }

    // Find the two histogram peaks by scanning left/right of the mid value.
    // The mid value is approximated as (max + min) / 2.
    const uint8_t mid_adc = (uint8_t)(((int)r.max + (int)r.min) / 2);

    // Scan lower half (ADC values 0..mid_adc) for the most frequent value
    // → this is the high-voltage steady-state → stored in `low`.
    uint32_t best_lower_count = 0;
    uint8_t  best_lower_val   = r.min;
    for (int v = 0; v <= mid_adc; v++) {
        if (hist[v] > best_lower_count) {
            best_lower_count = hist[v];
            best_lower_val   = (uint8_t)v;
        }
    }

    // Scan upper half (ADC values mid_adc+1..255) for the most frequent value
    // → this is the low-voltage steady-state → stored in `high`.
    uint32_t best_upper_count = 0;
    uint8_t  best_upper_val   = r.max;
    for (int v = mid_adc + 1; v < 256; v++) {
        if (hist[v] > best_upper_count) {
            best_upper_count = hist[v];
            best_upper_val   = (uint8_t)v;
        }
    }

    // If no clear bimodal distribution is found (one half has no samples),
    // fall back to using max/min as the steady-state levels.
    if (best_lower_count == 0) {
        best_lower_val = r.min;
    }
    if (best_upper_count == 0) {
        best_upper_val = r.max;
    }

    r.high = best_upper_val;  // larger ADC value = low-voltage level
    r.low  = best_lower_val;  // smaller ADC value = high-voltage level

    // Guard: if high and low are the same (flat signal), no edges can be
    // detected — leave level_valid=false so all time measurements report
    // "--" (matches the original behavior for non-periodic signals).
    if (r.high == r.low) {
        return;
    }

    // -- Step 2: Mid-threshold and edge detection --
    // Threshold is the midpoint of high and low ADC values.
    const double threshold = ((double)r.high + (double)r.low) / 2.0;

    // 10% and 90% levels for rise/fall time computation.
    // In ADC space: 10% of swing from `low` (high-voltage) toward `high`
    // (low-voltage). The 10% point is near `low` (high-voltage side),
    // the 90% point is near `high` (low-voltage side).
    const double swing = (double)r.high - (double)r.low;
    const double v10 = (double)r.low + 0.1 * swing;  // 10% from high-voltage
    const double v90 = (double)r.low + 0.9 * swing;  // 90% from high-voltage

    // Scan for edges: a rising edge (voltage rising = ADC value falling)
    // occurs when the signal crosses threshold from above. A falling edge
    // (voltage falling = ADC value rising) occurs when the signal crosses
    // threshold from below.
    //
    // In ADC space:
    //   Rising voltage edge: sample[i-1] > threshold >= sample[i]
    //     (ADC value decreases through threshold)
    //   Falling voltage edge: sample[i-1] < threshold <= sample[i]
    //     (ADC value increases through threshold)

    struct Edge {
        uint64_t index;  // sample index of the crossing
        bool     rising;  // true = rising voltage edge
    };

    std::vector<Edge> edges;
    // Pre-allocate generously: a 20K-sample DSO buffer typically has < 50
    // edges. Reserve 256 to avoid reallocation in the common case.
    edges.reserve(256);

    bool was_above = (double)samples[0] > threshold;

    for (uint64_t i = 1; i < sample_count; i++) {
        const bool is_above = (double)samples[i] > threshold;
        if (was_above && !is_above) {
            // Crossing from above → ADC value decreased → voltage rising
            edges.push_back({i, true});
        } else if (!was_above && is_above) {
            // Crossing from below → ADC value increased → voltage falling
            edges.push_back({i, false});
        }
        was_above = is_above;
    }

    // Need at least 2 rising edges to compute a period.
    if (edges.size() < 2) {
        return;
    }

    // -- Step 3: Period and pulse width --
    // Period: average distance between consecutive rising edges.
    // High pulse width: average distance from rising edge to next falling edge.
    double period_sum = 0.0;
    double high_time_sum = 0.0;
    uint32_t period_count = 0;
    uint32_t high_time_count = 0;

    for (size_t i = 0; i < edges.size(); i++) {
        if (!edges[i].rising) continue;
        // Find the next rising edge
        for (size_t j = i + 1; j < edges.size(); j++) {
            if (edges[j].rising) {
                period_sum += (double)(edges[j].index - edges[i].index);
                period_count++;
                break;
            }
        }
        // Find the next falling edge after this rising edge
        for (size_t j = i + 1; j < edges.size(); j++) {
            if (!edges[j].rising) {
                high_time_sum += (double)(edges[j].index - edges[i].index);
                high_time_count++;
                break;
            }
        }
    }

    if (period_count == 0) {
        return;
    }

    r.period     = (period_sum / period_count) * ns_per_sample;
    r.high_time  = (high_time_count > 0)
                       ? (high_time_sum / high_time_count) * ns_per_sample
                       : 0.0;
    r.pcount    = period_count;

    // -- Step 4: Rise/Fall time --
    // Rise time: on a rising voltage edge (ADC decreasing), find where the
    // signal crosses v90 (near low-voltage side, larger ADC) then v10 (near
    // high-voltage side, smaller ADC). Time = (v10_index - v90_index) * ns.
    //
    // Fall time: on a falling voltage edge (ADC increasing), find where the
    // signal crosses v10 (near high-voltage side, smaller ADC) then v90
    // (near low-voltage side, larger ADC). Time = (v90_index - v10_index) * ns.

    double rise_sum = 0.0;
    uint32_t rise_count = 0;
    double fall_sum = 0.0;
    uint32_t fall_count = 0;

    for (size_t i = 0; i < edges.size(); i++) {
        const Edge &e = edges[i];
        // Search backward from the edge for the 10%/90% crossing points.
        // We look in a window of at most `period_samples` samples before the
        // edge to avoid scanning the entire buffer for each edge.
        const double period_samples = period_sum / period_count;
        const uint64_t window = (uint64_t)std::max(period_samples * 0.5, 2.0);
        const uint64_t search_start = (e.index >= window) ? (e.index - window) : 0;

        if (e.rising) {
            // Rising voltage = ADC value decreasing.
            // Signal goes from high-ADC (low voltage) → low-ADC (high voltage).
            // Crosses v90 (larger ADC) first, then v10 (smaller ADC).
            int64_t v90_idx = -1;
            int64_t v10_idx = -1;
            for (uint64_t j = e.index; j > search_start; j--) {
                const double prev = (double)samples[j - 1];
                const double curr = (double)samples[j];
                if (v90_idx < 0 && prev >= v90 && curr < v90) {
                    v90_idx = (int64_t)j;
                }
                if (v90_idx >= 0 && prev >= v10 && curr < v10) {
                    v10_idx = (int64_t)j;
                    break;
                }
            }
            if (v90_idx >= 0 && v10_idx >= 0 && v10_idx > v90_idx) {
                rise_sum += (double)(v10_idx - v90_idx) * ns_per_sample;
                rise_count++;
            }
        } else {
            // Falling voltage = ADC value increasing.
            // Signal goes from low-ADC (high voltage) → high-ADC (low voltage).
            // Crosses v10 (smaller ADC) first, then v90 (larger ADC).
            int64_t v10_idx = -1;
            int64_t v90_idx = -1;
            for (uint64_t j = e.index; j > search_start; j--) {
                const double prev = (double)samples[j - 1];
                const double curr = (double)samples[j];
                if (v10_idx < 0 && prev <= v10 && curr > v10) {
                    v10_idx = (int64_t)j;
                }
                if (v10_idx >= 0 && prev <= v90 && curr > v90) {
                    v90_idx = (int64_t)j;
                    break;
                }
            }
            if (v10_idx >= 0 && v90_idx >= 0 && v90_idx > v10_idx) {
                fall_sum += (double)(v90_idx - v10_idx) * ns_per_sample;
                fall_count++;
            }
        }
    }

    r.rise_time = (rise_count > 0) ? rise_sum / rise_count : 0.0;
    r.fall_time = (fall_count > 0) ? fall_sum / fall_count : 0.0;

    // -- Step 5: Burst time --
    // Total time span from the first edge to the last edge.
    if (edges.size() >= 2) {
        r.burst_time = (double)(edges.back().index - edges.front().index)
                       * ns_per_sample;
    }

    // Level detection succeeded — at least one full period was found.
    r.level_valid = true;
}

// ---------------------------------------------------------------------------
// to_measurement_values() — convert raw result → API MeasurementValue list
//
// This is the Core-layer port of the switch statement in
// view::DsoMeasure::get_measure(int type) (dso_measure.cpp L47-L183).
// Each case applies the same formula; voltage values are converted from
// raw ADC counts to millivolts via convert_voltage().
// ---------------------------------------------------------------------------

std::vector<api::MeasurementValue>
MeasureCalculator::to_measurement_values(const MeasurementResult &r,
                                         double data_scale,
                                         uint64_t measure_vf,
                                         uint64_t vfactor,
                                         int view_rect_height)
{
    std::vector<api::MeasurementValue> out;

    auto make = [&](int type, double value, const char *unit,
                    bool valid) -> void {
        api::MeasurementValue mv;
        mv.type = type;
        mv.value = value;
        mv.unit = unit;
        mv.valid = valid;
        out.push_back(mv);
    };

    // Helper to convert raw ADC delta to millivolts.
    auto to_mv = [&](double raw_adc) -> double {
        return convert_voltage(raw_adc, data_scale, measure_vf, vfactor,
                               view_rect_height);
    };

    // Matches the original switch in DsoMeasure::get_measure(int type).
    // Voltage measurements use get_voltage(delta, 2) → millivolts.
    // Percentage measurements are dimensionless ratios * 100.
    // Time measurements are in nanoseconds (original get_time input).
    // Frequency is derived from period (1e9 / period_ns = Hz).

    // DSO_MS_AMPT — amplitude (high - low), level-dependent
    make(DSO_MS_AMPT,
         r.level_valid ? to_mv((double)r.high - (double)r.low) : 0.0,
         "mV", r.level_valid && r.mValid);

    // DSO_MS_VHIG — high voltage (hw_offset - low), level-dependent
    make(DSO_MS_VHIG,
         r.level_valid ? to_mv((double)r.hw_offset - (double)r.low) : 0.0,
         "mV", r.level_valid && r.mValid);

    // DSO_MS_VLOW — low voltage (hw_offset - high), level-dependent
    make(DSO_MS_VLOW,
         r.level_valid ? to_mv((double)r.hw_offset - (double)r.high) : 0.0,
         "mV", r.level_valid && r.mValid);

    // DSO_MS_VP2P — peak-to-peak (max - min)
    make(DSO_MS_VP2P,
         r.mValid ? to_mv((double)r.max - (double)r.min) : 0.0,
         "mV", r.mValid);

    // DSO_MS_VMAX — max voltage (hw_offset - min)
    make(DSO_MS_VMAX,
         r.mValid ? to_mv((double)r.hw_offset - (double)r.min) : 0.0,
         "mV", r.mValid);

    // DSO_MS_VMIN — min voltage (hw_offset - max)
    make(DSO_MS_VMIN,
         r.mValid ? to_mv((double)r.hw_offset - (double)r.max) : 0.0,
         "mV", r.mValid);

    // DSO_MS_PERD — period (ns), level-dependent
    make(DSO_MS_PERD, r.period, "ns", r.level_valid && r.mValid);

    // DSO_MS_FREQ — frequency (Hz), derived from period.
    // Original: 1e9 / period_ns = Hz (period is in ns).
    make(DSO_MS_FREQ,
         (r.level_valid && r.period != 0.0) ? 1.0e9 / r.period : 0.0,
         "Hz", r.level_valid && r.mValid && r.period != 0.0);

    // DSO_MS_VRMS — RMS voltage (millivolts)
    make(DSO_MS_VRMS,
         r.mValid ? to_mv(r.rms) : 0.0,
         "mV", r.mValid);

    // DSO_MS_VMEA — mean voltage (millivolts)
    make(DSO_MS_VMEA,
         r.mValid ? to_mv(r.mean) : 0.0,
         "mV", r.mValid);

    // DSO_MS_NOVR — overshoot ((max - high) * 100 / (high - low)), level-dependent
    make(DSO_MS_NOVR,
         (r.level_valid && (r.high - r.low != 0))
             ? ((double)r.max - (double)r.high) * 100.0
                   / ((double)r.high - (double)r.low)
             : 0.0,
         "%", r.level_valid && r.mValid && (r.high - r.low != 0));

    // DSO_MS_POVR — preshoot ((low - min) * 100 / (high - low)), level-dependent
    make(DSO_MS_POVR,
         (r.level_valid && (r.high - r.low != 0))
             ? ((double)r.low - (double)r.min) * 100.0
                   / ((double)r.high - (double)r.low)
             : 0.0,
         "%", r.level_valid && r.mValid && (r.high - r.low != 0));

    // DSO_MS_PDUT — positive duty (high_time / period * 100), level-dependent
    make(DSO_MS_PDUT,
         (r.level_valid && r.period != 0.0)
             ? r.high_time / r.period * 100.0
             : 0.0,
         "%", r.level_valid && r.mValid && r.period != 0.0);

    // DSO_MS_NDUT — negative duty (100 - high_time / period * 100), level-dependent
    make(DSO_MS_NDUT,
         (r.level_valid && r.period != 0.0)
             ? 100.0 - r.high_time / r.period * 100.0
             : 0.0,
         "%", r.level_valid && r.mValid && r.period != 0.0);

    // DSO_MS_PWDT — positive pulse width (high_time, ns), level-dependent
    make(DSO_MS_PWDT, r.high_time, "ns", r.level_valid && r.mValid);

    // DSO_MS_NWDT — negative pulse width (period - high_time, ns), level-dependent
    make(DSO_MS_NWDT,
         r.level_valid ? (r.period - r.high_time) : 0.0,
         "ns", r.level_valid && r.mValid);

    // DSO_MS_RISE — rise time (ns), level-dependent
    make(DSO_MS_RISE, r.rise_time, "ns", r.level_valid && r.mValid);

    // DSO_MS_FALL — fall time (ns), level-dependent
    make(DSO_MS_FALL, r.fall_time, "ns", r.level_valid && r.mValid);

    // DSO_MS_BRST — burst time (ns), level-dependent
    make(DSO_MS_BRST, r.burst_time, "ns", r.level_valid && r.mValid);

    // DSO_MS_PCNT — pulse count, level-dependent
    make(DSO_MS_PCNT, (double)r.pcount, "",
         r.level_valid && r.mValid);

    return out;
}

// ---------------------------------------------------------------------------
// Formatting helpers (reused by view::DsoMeasure for QString output)
// ---------------------------------------------------------------------------

double MeasureCalculator::convert_voltage(double raw_adc,
                                          double data_scale,
                                          uint64_t measure_vf,
                                          uint64_t vfactor,
                                          int view_rect_height)
{
    if (view_rect_height <= 0) {
        view_rect_height = DefaultViewRectHeight;
    }
    // Same formula as DsoMeasure::get_voltage(double v, int p, scaled=false):
    //   v_mV = v * data_scale * k * vDial_factor * DS_CONF_DSO_VDIVS
    //          / view_rect_height
    return raw_adc * data_scale * (double)measure_vf * (double)vfactor
           * (double)DS_CONF_DSO_VDIVS / (double)view_rect_height;
}

QString MeasureCalculator::format_voltage(double v_mv, int precision)
{
    // Matches DsoMeasure::get_voltage(double v, int p) return formatting:
    // abs(v) >= 1000 ? "X.XXV" : "X.XXmV"
    return std::abs(v_mv) >= 1000.0
               ? QString::number(v_mv / 1000.0, 'f', precision) + "V"
               : QString::number(v_mv, 'f', precision) + "mV";
}

QString MeasureCalculator::format_time(double t_ns)
{
    // Matches DsoMeasure::get_time formatting:
    // abs(t) > 1e9 ? "X.XXS" : > 1e6 ? "X.XXmS" : > 1e3 ? "X.XXuS" : "X.XXnS"
    return (std::abs(t_ns) > 1000000000.0
                ? QString::number(t_ns / 1000000000.0, 'f', 2) + "S"
            : std::abs(t_ns) > 1000000.0
                ? QString::number(t_ns / 1000000.0, 'f', 2) + "mS"
            : std::abs(t_ns) > 1000.0
                ? QString::number(t_ns / 1000.0, 'f', 2) + "uS"
                : QString::number(t_ns, 'f', 2) + "nS");
}

QString MeasureCalculator::format_frequency(double period_ns)
{
    // Matches DSO_MS_FREQ case in DsoMeasure::get_measure:
    if (period_ns == 0.0) {
        return "--";
    }
    if (std::abs(period_ns) > 1000000.0) {
        return QString::number(1000000000.0 / period_ns, 'f', 2) + "Hz";
    } else if (std::abs(period_ns) > 1000.0) {
        return QString::number(1000000.0 / period_ns, 'f', 2) + "kHz";
    } else {
        return QString::number(1000.0 / period_ns, 'f', 2) + "MHz";
    }
}

} // namespace core
} // namespace pv
