#ifndef PXVIEW_CORE_MEASURECALCULATOR_H
#define PXVIEW_CORE_MEASURECALCULATOR_H

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

#include <cstdint>
#include <memory>
#include <vector>

#include <QString>

#include "../api/types.h"        // api::MeasurementValue (Core-layer API type)
#include "../data/signalmodel.h" // data::SignalModel (Core-layer channel model)
#include "../dsvdef.h"           // DSO_MS_* / DS_CONF_DSO_VDIVS

namespace pv {

class SessionData;

namespace core {

/**
 * MeasureCalculator — Core-layer DSO measurement computation.
 *
 * Extracted from view/dso_measure.cpp (Task C1 of
 * plan-core-view-split-and-mcp-coverage). The measurement formatting logic
 * (the switch statement in DsoMeasure::get_measure(int type)) is moved here
 * so that headless mode (no View layer loaded) can compute real measurement
 * values via SessionService::get_measurements() -> SigSession::get_measurements()
 * -> MeasureCalculator::compute().
 *
 * The View layer (view::DsoMeasure) retains the QPainter display logic
 * (paint_hover_measure) and the hover-point pick logic (measure/get_point);
 * its get_measure(int type) forwards here via DataSource::get_measurements().
 *
 * Algorithm equivalence: the formulas in to_measurement_values() are copied
 * verbatim from the original DsoMeasure::get_measure(int type) switch
 * (dso_measure.cpp L47-L183). The raw value computation (max/min/rms/mean)
 * uses DsoSnapshot's existing Core-layer methods (get_max_min_value /
 * cal_vrms / cal_vmean). Level-dependent measurements (period/freq/duty/
 * rise/fall/high/low) were never computed in the original code (DsoSignal
 *::_mValid was never set to true, and the level-detection routine was
 * removed — see capturemanager.cpp:326 TODO), so they are reported with
 * valid=false, matching the original "--" behavior.
 */
class MeasureCalculator
{
public:
    /**
     * Raw measurement result for a single DSO channel.
     * Mirrors the private fields of view::DsoSignal that the original
     * DsoMeasure::get_measure(int type) read (_max/_min/_high/_low/_period/
     * _rms/_mean/_rise_time/_fall_time/_high_time/_burst_time/_pcount/
     * _level_valid/_mValid/_hw_offset).
     */
    struct MeasurementResult
    {
        int      channel_index = -1;  ///< sr_channel index
        bool     mValid = false;      ///< max/min/rms/mean computed successfully
        bool     level_valid = false; ///< level-dependent measurements valid
        uint8_t  max = 0;             ///< max ADC value
        uint8_t  min = 0;             ///< min ADC value
        uint8_t  high = 0;            ///< high level (not computed — level_valid=false)
        uint8_t  low = 0;             ///< low level (not computed — level_valid=false)
        double   period = 0;          ///< period in ns (not computed)
        double   rms = 0;             ///< RMS in ADC counts
        double   mean = 0;            ///< mean in ADC counts
        double   rise_time = 0;       ///< rise time in ns (not computed)
        double   fall_time = 0;       ///< fall time in ns (not computed)
        double   high_time = 0;       ///< high pulse width in ns (not computed)
        double   burst_time = 0;      ///< burst time in ns (not computed)
        uint32_t pcount = 0;          ///< pulse count (not computed)
        int      hw_offset = 0;       ///< hardware zero offset (ADC counts)
    };

    /**
     * Compute raw measurements for one DSO channel (channel_index) or all
     * enabled DSO channels (channel_index == -1) from the SessionData's
     * DsoSnapshot.
     *
     * For each channel: reads max/min via DsoSnapshot::get_max_min_value,
     * rms via DsoSnapshot::cal_vrms, mean via DsoSnapshot::cal_vmean, and
     * hw_offset via SignalModel. Sets mValid=true if max/min/rms/mean were
     * computed. level_valid stays false (level detection not implemented —
     * matches original behavior where _mValid was never set true).
     *
     * @param data            SessionData (must outlive the call); its
     *                        DsoSnapshot is read under the snapshot's mutex.
     * @param signal_models   SignalModel list (from SessionStateContext) —
     *                        used to look up hw_offset per DSO channel.
     * @param channel_index   sr_channel index, or -1 for all DSO channels.
     * @param view_rect_height Pixel height of the DSO trace (used downstream
     *                        by to_measurement_values for voltage conversion
     *                        — pass 0 to use the default headless height).
     * @return Vector of MeasurementResult (one per requested channel; empty
     *         if no DSO data or snapshot is empty).
     */
    static std::vector<MeasurementResult> compute(
        SessionData *data,
        const std::vector<std::shared_ptr<data::SignalModel>> &signal_models,
        int channel_index = -1,
        int view_rect_height = 0);

    /**
     * Convert a raw MeasurementResult into a list of API MeasurementValue
     * structs (one per DSO_MS_* measurement type). Applies the same formula
     * switch as the original DsoMeasure::get_measure(int type):
     *   - Voltage measurements (VMAX/VMIN/VP2P/VRMS/VMEA/VHIG/VLOW/AMPT)
     *     are converted from raw ADC counts to millivolts using the same
     *     data_scale * measure_vf * vfactor * DS_CONF_DSO_VDIVS / height
     *     formula.
     *   - Percentage measurements (PDUT/NDUT/NOVR/POVR) use the same
     *     ratio formulas.
     *   - Time measurements (PERD/PWDT/NWDT/RISE/FALL/BRST) use the same
     *     get_time-style values (in nanoseconds).
     *   - Frequency (FREQ) uses the same period -> Hz/kHz/MHz conversion.
     *   - Pulse count (PCNT) uses the same count formatting.
     *
     * Each output MeasurementValue.value is in the base unit (mV / % / Hz /
     * ns) and MeasurementValue.unit is the SI base unit string. Callers
     * (View layer or MCP JSON serializer) may rescale for display.
     *
     * @param r              The raw measurement result.
     * @param data_scale     vdiv (mV per division) from SignalModel.
     * @param measure_vf     measure_voltage_factor from DsoSnapshot (vfactor).
     * @param vfactor        vDial probe factor (1/10/100).
     * @param view_rect_height Pixel height of the trace (0 = use default).
     * @return Vector of MeasurementValue (one per DSO_MS_* type, in enum
     *         order; entries with valid=false when the underlying value is
     *         not available).
     */
    static std::vector<api::MeasurementValue> to_measurement_values(
        const MeasurementResult &r,
        double data_scale,
        uint64_t measure_vf,
        uint64_t vfactor,
        int view_rect_height);

    // ---- Formatting helpers (reused by view::DsoMeasure for QString output) ----

    /**
     * Convert a raw ADC value (or ADC delta) to millivolts using the same
     * formula as DsoMeasure::get_voltage(double v, int p, bool scaled=false):
     *   v_mV = v * data_scale * measure_vf * vfactor * DS_CONF_DSO_VDIVS
     *          / view_rect_height
     */
    static double convert_voltage(double raw_adc,
                                  double data_scale,
                                  uint64_t measure_vf,
                                  uint64_t vfactor,
                                  int view_rect_height);

    /**
     * Format a millivolt value as a display string ("1.23V" / "456.7mV"),
     * matching DsoMeasure::get_voltage(double v, int p) formatting.
     */
    static QString format_voltage(double v_mv, int precision = 2);

    /**
     * Format a nanosecond value as a display string ("1.23S" / "45.6mS" /
     * "789.0uS" / "12.0nS"), matching DsoMeasure::get_time formatting.
     */
    static QString format_time(double t_ns);

    /**
     * Format a frequency from a period (nanoseconds) as a display string
     * ("1.23Hz" / "4.56kHz" / "7.89MHz"), matching the DSO_MS_FREQ case
     * in DsoMeasure::get_measure.
     */
    static QString format_frequency(double period_ns);

    /**
     * Default trace pixel height used for voltage conversion when no View
     * geometry is available (headless mode). The original View code used
     * DsoSignal::get_view_rect().height(); in headless we fall back to
     * DS_CONF_DSO_VDIVS * DefaultPixelsPerDiv so the voltage formula stays
     * dimensionally consistent.
     */
    static constexpr int DefaultViewRectHeight = 256;
    static constexpr int DefaultPixelsPerDiv = 32;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_MEASURECALCULATOR_H
