/*
 * This file is part of the PXView project.
 * PXView is based on PulseView.
 *
 * Natural order string comparison — ported from PulseView's strnatcmp.hpp
 * (originally by Martin Pool <mbp sourcefrog net>).
 *
 * When sorting channel names like "CH2", "CH10", "CH1", natural ordering
 * produces "CH1", "CH2", "CH10" whereas lexicographic strcmp produces
 * "CH1", "CH10", "CH2".
 *
 * License: public-domain-compatible (original license below preserved).
 */

#ifndef PXVIEW_PV_BASE_STRNATCMP_H
#define PXVIEW_PV_BASE_STRNATCMP_H

#include <cctype>
#include <cstddef>
#include <string>

namespace pv {
namespace base {

static inline int compare_right(char const *a, char const *b)
{
    int bias = 0;

    for (;; a++, b++) {
        if (!isdigit(static_cast<unsigned char>(*a)) && !isdigit(static_cast<unsigned char>(*b)))
            return bias;
        if (!isdigit(static_cast<unsigned char>(*a)))
            return -1;
        if (!isdigit(static_cast<unsigned char>(*b)))
            return +1;

        if (*a < *b) {
            if (!bias)
                bias = -1;
        } else if (*a > *b) {
            if (!bias)
                bias = +1;
        } else if (!*a && !*b)
            return bias;
    }
    return 0;
}

static inline int compare_left(char const *a, char const *b)
{
    for (;; a++, b++) {
        if (!isdigit(static_cast<unsigned char>(*a)) && !isdigit(static_cast<unsigned char>(*b)))
            return 0;
        if (!isdigit(static_cast<unsigned char>(*a)))
            return -1;
        if (!isdigit(static_cast<unsigned char>(*b)))
            return +1;
        if (*a < *b)
            return -1;
        if (*a > *b)
            return +1;
    }
    return 0;
}

static inline int strnatcmp0(char const *a, char const *b, int fold_case)
{
    int ai, bi, fractional, result;
    char ca, cb;

    ai = bi = 0;

    while (true) {
        ca = a[ai];
        cb = b[bi];

        while (isspace(static_cast<unsigned char>(ca)))
            ca = a[++ai];
        while (isspace(static_cast<unsigned char>(cb)))
            cb = b[++bi];

        if (isdigit(static_cast<unsigned char>(ca)) && isdigit(static_cast<unsigned char>(cb))) {
            fractional = (ca == '0' || cb == '0');
            if (fractional) {
                if ((result = compare_left(a + ai, b + bi)) != 0)
                    return result;
            } else {
                if ((result = compare_right(a + ai, b + bi)) != 0)
                    return result;
            }
        }

        if (!ca && !cb)
            return 0;

        if (fold_case) {
            ca = static_cast<char>(toupper(static_cast<unsigned char>(ca)));
            cb = static_cast<char>(toupper(static_cast<unsigned char>(cb)));
        }

        if (ca < cb)
            return -1;
        if (ca > cb)
            return +1;

        ++ai;
        ++bi;
    }
}

/// Natural-order comparison, case-sensitive.
/// "CH2" < "CH10" (unlike strcmp where "CH10" < "CH2").
inline int strnatcmp(char const *a, char const *b)
{
    return strnatcmp0(a, b, 0);
}

/// Natural-order comparison, case-sensitive, std::string overload.
inline int strnatcmp(const std::string &a, const std::string &b)
{
    return strnatcmp0(a.c_str(), b.c_str(), 0);
}

/// Natural-order comparison, case-insensitive.
inline int strnatcasecmp(char const *a, char const *b)
{
    return strnatcmp0(a, b, 1);
}

/// Natural-order comparison, case-insensitive, std::string overload.
inline int strnatcasecmp(const std::string &a, const std::string &b)
{
    return strnatcmp0(a.c_str(), b.c_str(), 1);
}

/// Functor for use with std::sort on containers of strings.
struct NaturalCompare {
    bool operator()(const std::string &a, const std::string &b) const
    {
        return strnatcmp(a, b) < 0;
    }
};

/// Case-insensitive functor.
struct NaturalCompareCI {
    bool operator()(const std::string &a, const std::string &b) const
    {
        return strnatcasecmp(a, b) < 0;
    }
};

} // namespace base
} // namespace pv

#endif // PXVIEW_PV_BASE_STRNATCMP_H
