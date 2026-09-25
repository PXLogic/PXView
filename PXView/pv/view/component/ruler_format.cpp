/*
 * ruler_format.cpp — Ruler formatting helpers implementation
 *
 * Extracted from ruler.cpp L142-L229.
 * These are pure functions — no QWidget/View/AppConfig dependencies.
 */

#include "ruler_format.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits.h>

namespace pv {
namespace view {

// SI prefix arrays (from ruler.cpp L55-L60)
static const QString SIPrefixes[9] =
    {"f", "p", "n", QChar(0x03BC), "m", "", "k", "M", "G"};
static const QString FreqPrefixes[9] =
    {"P", "T", "G", "M", "K", "", "", "", ""};

QString format_freq(double period, unsigned int precision)
{
    if (period <= 0) {
        return kUnknownStr;
    }
    const int order = static_cast<int>(ceil(log10f(static_cast<float>(period))));
    const int prefix = static_cast<int>(ceil(static_cast<float>((order - kFirstSIPrefixPower)) / 3.0f));
    const double multiplier = pow(10.0, std::max(-prefix * 3.0 - static_cast<double>(kFirstSIPrefixPower), 0.0));

    const int p = static_cast<int>(std::min(precision, 12u));
    char buffer[50] = {0};
    QString units = FreqPrefixes[prefix] + "Hz";
    snprintf(buffer, sizeof(buffer), "%.*f%s", p, 1 / (period * multiplier),
             units.toUtf8().constData());
    return QString(buffer);
}

QString format_time(double t, int prefix, unsigned int precision)
{
    const double multiplier = pow(10.0, -prefix * 3 - kFirstSIPrefixPower + 6.0);

    const int p = static_cast<int>(std::min(precision, 12u));
    char buffer[50];
    QString units = SIPrefixes[prefix] + "s";
    double v = (t * multiplier) / 1000000.0;
    snprintf(buffer, sizeof(buffer), "%c%.*f%s", v >= 0 ? '+' : '-', p, v,
             units.toUtf8().constData());
    return QString(buffer);
}

QString format_real_time(uint64_t delta_index, uint64_t sample_rate)
{
    double v1 = static_cast<double>(std::pow(10, 12)) / static_cast<double>(sample_rate);
    double delta_time_double = v1 * static_cast<double>(delta_index);
    uint64_t delta_time = static_cast<unsigned long>(v1 * static_cast<double>(delta_index));

    // static_cast, not a bare comparison against UINT64_MAX: the macro is a
    // uint64_t, so `double > uint64_t` converts it to double anyway (2^64,
    // since 2^64-1 is not representable) and gcc flags the silent value change.
    // Making the conversion explicit keeps the existing behaviour.
    if (delta_time_double > static_cast<double>(UINT64_MAX)) {
        return "INF";
    }
    if (delta_time == 0) {
        return "0";
    }

    int zero = 0;
    int prefix = static_cast<int>(floor(log10(static_cast<double>(delta_time))));
    while (delta_time == (delta_time / 10 * 10)) {
        delta_time /= 10;
        zero++;
    }

    return format_time(static_cast<double>(delta_time) / std::pow(10.0, 12 - zero),
                       prefix / 3 + 1,
                       prefix / 3 * 3 > zero ? prefix / 3 * 3 - zero : 0);
}

QString format_real_freq(uint64_t delta_index, uint64_t sample_rate)
{
    const double delta_period = static_cast<double>(delta_index) * 1.0 / static_cast<double>(sample_rate);
    return format_freq(delta_period);
}

// Per-cursor HSV hue table (verbatim from ruler.cpp CursorHsbColorTable,
// CURSOR_HSB_COLOR_TABLE_LENGTH entries).
static const int kCursorHsbColorTable[22] = {
    120,
    195,
    270,
    345,
    60, //5
    135,
    210,
    285,
    15,
    75, //10
    150,
    225,
    300,
    30,
    90, //15
    165,
    240,
    315,
    45,
    105, //20
    180,
    255,
};

QColor cursor_hsb_color(int order)
{
    assert(order > 0);

    constexpr int kTableLength = sizeof(kCursorHsbColorTable) / sizeof(int);
    int hsv = kCursorHsbColorTable[(order - 1) % kTableLength];
    QColor color;

    int b = 200; // IsDarkStyle() ? 200 : 200 — both branches identical
    color.setHsv(hsv, 200, b, 180);
    return color;
}

} // namespace view
} // namespace pv
