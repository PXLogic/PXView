/*
 * measure_format.cpp — DSO measurement formatting helpers implementation
 *
 * Extracted from measurecalculator.cpp L565-L616.
 * These are pure functions — no Snapshot/SessionData dependencies.
 */

#include "measure_format.h"

#include <cmath>

namespace pv {
namespace core {

double convert_voltage(double raw_adc,
                       double data_scale,
                       uint64_t measure_vf,
                       uint64_t vfactor,
                       int view_rect_height)
{
    if (view_rect_height <= 0) {
        view_rect_height = kDefaultViewRectHeight;
    }
    return raw_adc * data_scale * (double)measure_vf * (double)vfactor
           * (double)kDsoVdivs / (double)view_rect_height;
}

QString format_voltage(double v_mv, int precision)
{
    return std::abs(v_mv) >= 1000.0
               ? QString::number(v_mv / 1000.0, 'f', precision) + "V"
               : QString::number(v_mv, 'f', precision) + "mV";
}

QString format_time(double t_ns)
{
    return (std::abs(t_ns) > 1000000000.0
                ? QString::number(t_ns / 1000000000.0, 'f', 2) + "S"
            : std::abs(t_ns) > 1000000.0
                ? QString::number(t_ns / 1000000.0, 'f', 2) + "mS"
            : std::abs(t_ns) > 1000.0
                ? QString::number(t_ns / 1000.0, 'f', 2) + "uS"
                : QString::number(t_ns, 'f', 2) + "nS");
}

QString format_frequency(double period_ns)
{
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
