/*
 * ruler_format.cpp — Ruler formatting helpers implementation
 *
 * Extracted from ruler.cpp L142-L229.
 * These are pure functions — no QWidget/View/AppConfig dependencies.
 */

#include "ruler_format.h"

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
    const int order = ceil(log10f(period));
    const int prefix = ceil((order - kFirstSIPrefixPower) / 3.0f);
    const double multiplier = pow(10.0, std::max(-prefix * 3.0 - (double)kFirstSIPrefixPower, 0.0));

    char buffer[50] = {0};
    char format[15] = {0};
    QString units = FreqPrefixes[prefix] + "Hz";
    sprintf(format, "%%.%df", (int)precision);
    sprintf(buffer, format, 1 / (period * multiplier));
    strcat(buffer, units.toUtf8().data());
    return QString(buffer);
}

QString format_time(double t, int prefix, unsigned int precision)
{
    const double multiplier = pow(10.0, -prefix * 3 - kFirstSIPrefixPower + 6.0);

    char buffer[50];
    char format[15];
    QString units = SIPrefixes[prefix] + "s";
    double v = (t * multiplier) / 1000000.0;
    buffer[0] = v >= 0 ? '+' : '-';
    sprintf(format, "%%.%df", (int)precision);
    sprintf(buffer + 1, format, v);
    strcat(buffer + 1, units.toUtf8().data());
    return QString(buffer);
}

QString format_real_time(uint64_t delta_index, uint64_t sample_rate)
{
    double v1 = (double)std::pow(10, 12) / (double)sample_rate;
    double delta_time_double = v1 * delta_index;
    uint64_t delta_time = v1 * delta_index;

    if (delta_time_double > UINT64_MAX) {
        return "INF";
    }
    if (delta_time == 0) {
        return "0";
    }

    int zero = 0;
    int prefix = (int)floor(log10(delta_time));
    while (delta_time == (delta_time / 10 * 10)) {
        delta_time /= 10;
        zero++;
    }

    return format_time(delta_time / std::pow(10.0, 12 - zero),
                       prefix / 3 + 1,
                       prefix / 3 * 3 > zero ? prefix / 3 * 3 - zero : 0);
}

QString format_real_freq(uint64_t delta_index, uint64_t sample_rate)
{
    const double delta_period = delta_index * 1.0 / sample_rate;
    return format_freq(delta_period);
}

} // namespace view
} // namespace pv
