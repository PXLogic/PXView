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

#include <cmath>
#include <cstdlib>

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
        // level_valid stays false: the level-detection algorithm (high/low
        // threshold, period, rise/fall time) was never implemented in the
        // current codebase (DsoSignal::_mValid was never set true). This
        // matches the original behavior where all level-dependent
        // measurements returned "--".
        r.level_valid = false;

        results.push_back(r);
    }

    return results;
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
