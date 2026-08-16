/*
 * ruler_format.h — Ruler formatting helpers (extracted for testability)
 *
 * These functions were originally static methods of Ruler
 * (ruler.cpp L142-L229). They are pure functions with no QWidget/View
 * dependencies, so they are extracted here to allow unit testing without
 * linking the full ruler.cpp translation unit (which pulls in
 * View/AppConfig/Cursor/Session dependencies).
 *
 * Ruler::format_real_time / format_real_freq / format_freq / format_time
 * delegate to these free functions.
 */

#ifndef PXVIEW_VIEW_RULER_FORMAT_H
#define PXVIEW_VIEW_RULER_FORMAT_H

#include <cstdint>
#include <QString>

namespace pv {
namespace view {

// SI prefix constants (from ruler.cpp L55-L60)
// "f", "p", "n", "μ", "m", "", "k", "M", "G"
// Frequency prefixes: "P", "T", "G", "M", "K", "", "", "", ""
static const int kFirstSIPrefixPower = -15;
static const QString kUnknownStr = "########";

QString format_freq(double period, unsigned int precision = 2);
QString format_time(double t, int prefix, unsigned int precision = 2);
QString format_real_time(uint64_t delta_index, uint64_t sample_rate);
QString format_real_freq(uint64_t delta_index, uint64_t sample_rate);

} // namespace view
} // namespace pv

#endif // PXVIEW_VIEW_RULER_FORMAT_H
