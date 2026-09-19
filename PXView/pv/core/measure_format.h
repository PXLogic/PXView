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

// ADC full-scale span for the DSO path: ref_max - ref_min.
//
// PXLogic / demo DSO channels are 8-bit (DsoSignal::DefaultBits == 8), so the
// raw ADC count range is 0..255 and the span is 255. This is the same constant
// that mathstack.cpp and spectrumstack.cpp already hard-code as `255.0`; it is
// named here so the three DSO voltage sites share one documented source.
//
// If a future device reports a different bit depth (DsoSignal::_bits comes from
// SR_CONF_UNIT_BITS / SR_CONF_REF_MIN / SR_CONF_REF_MAX), this must become
// device-derived rather than a compile-time constant.
constexpr int kAdcFullScale = 255;

// Raw ADC count -> normalised 0..1 amplitude (the reciprocal of the full-scale
// span). This is the value stored in DsoSnapshot::set_data_scale().
//
// History (devdoc/P0-P1-P2架构设计与实施计划.md §4.9.2): upstream DSView stored
// DsoSignal::get_scale() there, i.e. `height / (ref_max - ref_min) * stop_scale`.
// Every consumer then divided by the view height again, so the height cancelled
// and the net factor was `1 / (ref_max - ref_min)` — independent of the view
// geometry. The PXView port lost that cancellation (it stored `vdiv` instead),
// which is what produced the vfactor² / vdiv² defects. Storing the reciprocal
// directly makes the whole formula height-independent and removes the need for
// the View to push its geometry into Core.
constexpr double kAdcScale = 1.0 / (double)kAdcFullScale;

// ---- mV <-> V 单位边界 ----
//
// 项目里同时存在两种电压"档位"单位，且**没有**单一约定：
//   * 内部 / 驱动 / UI 路径 —— **mV/div**
//       - SR_CONF_PROBE_VDIV 的值（10..2000）
//       - dslDial 的取值列表（DsoSignal::init_vDial）
//       - .pxc 的 channel "vdiv" 字段
//       - SignalModel::vdiv_mv() 与 DsoSnapshot::get_measure_voltage_factor()
//       - core::convert_voltage() 的返回值（毫伏）
//   * 对外 API 路径 —— **V/div**
//       - api::ProbeConfig::vdiv（MCP / RPC 的 "vdiv" 参数，文档写 "Volts per division"）
//
// 历史缺陷：`SessionService::set_probe_config()` 曾把 API 的 V/div 直接写进
// mV/div 的 SignalModel 字段 —— 1 V/div 变成 1 mV/div，经
// `set_measure_voltage_factor((uint64_t)m->vdiv_mv())` 后 DSO 测量电压偏小 1000×。
// 现在跨边界只允许走下面这两个具名换算，不要再写裸 `* 1000.0`。
constexpr double kMvPerVolt = 1000.0;

inline double volts_to_millivolts(double v) { return v * kMvPerVolt; }
inline double millivolts_to_volts(double mv) { return mv / kMvPerVolt; }

/**
 * Convert a raw ADC value (or ADC delta) to millivolts.
 *
 *   v_mV = raw_adc * data_scale * measure_vf * vfactor * kDsoVdivs
 *
 * @param raw_adc     Raw ADC count (or count delta).
 * @param data_scale  DsoSnapshot data_scale — raw count -> 0..1 normalised
 *                    amplitude, i.e. kAdcScale (1/255). NOT the view scale.
 * @param measure_vf  DsoSnapshot measure_voltage_factor — the V/div dial step
 *                    in mV/div (SignalModel::vdiv_mv()).
 * @param vfactor     Probe attenuation factor (1 / 10 / 100), i.e.
 *                    SignalModel::vfactor().
 * @return Voltage in **millivolts** (use core::millivolts_to_volts() at any
 *         boundary that promises volts).
 *
 * There is deliberately no view_rect_height parameter: upstream's `/height`
 * existed only because its data_scale contained the height (see kAdcScale
 * above), so the two cancelled. Consumers that hold *pixel* coordinates rather
 * than ADC counts (view::DsoMeasure::get_voltage(v, p, scaled=true), used by
 * the cursors and the trigger-level readout) keep their own `/height`.
 */
double convert_voltage(double raw_adc,
                       double data_scale,
                       uint64_t measure_vf,
                       uint64_t vfactor);

QString format_voltage(double v_mv, int precision = 2);

QString format_time(double t_ns);

QString format_frequency(double period_ns);

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_MEASURE_FORMAT_H
