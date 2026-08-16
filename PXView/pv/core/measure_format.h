/*
 * measure_format.h — DSO measurement formatting helpers (extracted for testability)
 *
 * These functions were originally static methods of MeasureCalculator
 * (measurecalculator.cpp L565-L616). They are pure functions with no
 * dependencies on Snapshot/SessionData, so they are extracted here to
 * allow unit testing without linking the full MeasureCalculator translation
 * unit (which pulls in DsoSnapshot/Snapshot symbols).
 *
 * MeasureCalculator::convert_voltage / format_voltage / format_time /
 * format_frequency delegate to these free functions.
 */

#ifndef PXVIEW_CORE_MEASURE_FORMAT_H
#define PXVIEW_CORE_MEASURE_FORMAT_H

#include <cstdint>
#include <QString>

namespace pv {
namespace core {

constexpr int kDsoVdivs = 8;          // DS_CONF_DSO_VDIVS from pxvdef.h
constexpr int kDefaultViewRectHeight = 256;

double convert_voltage(double raw_adc,
                       double data_scale,
                       uint64_t measure_vf,
                       uint64_t vfactor,
                       int view_rect_height);

QString format_voltage(double v_mv, int precision = 2);

QString format_time(double t_ns);

QString format_frequency(double period_ns);

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_MEASURE_FORMAT_H
