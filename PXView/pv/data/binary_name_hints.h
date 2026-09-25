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

#ifndef PXVIEW_PV_DATA_BINARY_NAME_HINTS_H
#define PXVIEW_PV_DATA_BINARY_NAME_HINTS_H

#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <cstdint>

namespace pv {
namespace data {
namespace binary_name_hints {

/**
 * What a file name can tell us about a headerless raw binary capture.
 *
 * A raw ".binary" file has no header at all, so its two essential parameters —
 * the number of logic channels (which fixes the bytes-per-sample stride) and the
 * sample rate (which fixes the time axis) — used to be guesswork on import:
 * PXView fell back to "enabled logic channels of whatever device happens to be
 * open, else 8" and "current device rate, else 1 MHz". Encoding them in the
 * file name (as PXView's binary export now does) makes the pair round-trip
 * without a side-car file and works with files shared between machines.
 *
 * Written form: "<base>-<channels>ch-<samplerate>Hz.binary", e.g.
 * "Demo device-LA-260924-213100-32ch-1000000Hz.binary". Both tokens are parsed
 * independently, case-insensitively and in any order, so hand-renamed files
 * ("32ch" / "1MHz", "16CH-2khz") are understood as well.
 */
struct Hints {
    int channels = 0;         ///< 0 = not stated by the name
    uint64_t samplerate = 0;  ///< 0 = not stated by the name

    bool empty() const { return channels <= 0 && samplerate == 0; }
};

/**
 * Parse the hint tokens out of a file name (or a full path — the directory is
 * irrelevant and simply not matched by the tokens).
 */
inline Hints parse(const QString &file_name)
{
    Hints hints;

    // "<n>ch" — the lookahead keeps "channels" or "ch1" from matching.
    static const QRegularExpression channels_re(
        QStringLiteral("(\\d{1,6})\\s*ch(?![A-Za-z0-9])"),
        QRegularExpression::CaseInsensitiveOption);

    // "<n>[k|M|G]Hz" — the plain-Hz form PXView writes, plus the SI forms a
    // human is likely to type.
    static const QRegularExpression samplerate_re(
        QStringLiteral("(\\d+)\\s*([kKmMgG]?)\\s*hz(?![A-Za-z])"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch channels_match = channels_re.match(file_name);
    if (channels_match.hasMatch()) {
        bool ok = false;
        const int channels = channels_match.captured(1).toInt(&ok);
        if (ok && channels > 0)
            hints.channels = channels;
    }

    const QRegularExpressionMatch samplerate_match = samplerate_re.match(file_name);
    if (samplerate_match.hasMatch()) {
        bool ok = false;
        const qulonglong value = samplerate_match.captured(1).toULongLong(&ok);
        if (ok && value > 0) {
            qulonglong scale = 1;
            const QString prefix = samplerate_match.captured(2).toLower();
            if (prefix == QLatin1String("k"))
                scale = 1000ULL;
            else if (prefix == QLatin1String("m"))
                scale = 1000000ULL;
            else if (prefix == QLatin1String("g"))
                scale = 1000000000ULL;

            hints.samplerate = value * scale;
        }
    }

    return hints;
}

/**
 * Write the hint block into @p file_name, keeping any extension in place:
 * "x.binary" -> "x-32ch-1000000Hz.binary", "/d/name" -> "/d/name-32ch-1000000Hz".
 *
 * Idempotent — an existing hint block is replaced, not appended again, so
 * re-exporting over a name produced by an earlier export cannot accumulate
 * tokens. A token is only written when its value is known (nothing is invented).
 */
inline QString apply(const QString &file_name, int channels, uint64_t samplerate)
{
    if (file_name.isEmpty() || (channels <= 0 && samplerate == 0))
        return file_name;

    // Split off the extension of the last path component (a '.' in a directory
    // name must not be taken for one).
    // qsizetype, not int: QString::lastIndexOf() returns qsizetype and a path
    // longer than INT_MAX would otherwise be truncated here.
    const qsizetype separator = std::max(file_name.lastIndexOf(QLatin1Char('/')),
                                         file_name.lastIndexOf(QLatin1Char('\\')));
    const qsizetype dot = file_name.lastIndexOf(QLatin1Char('.'));
    const bool has_extension = dot > separator + 1;
    const QString base = has_extension ? file_name.left(dot) : file_name;
    const QString extension = has_extension ? file_name.mid(dot) : QString();

    // Drop whatever hint block the name already carries.
    QString cleaned = base;
    static const QRegularExpression hints_re(
        QStringLiteral("[-_]?\\d{1,6}ch[-_]?\\d+(?:[kKmMgG])?hz"),
        QRegularExpression::CaseInsensitiveOption);
    cleaned.remove(hints_re);

    QString block;
    if (channels > 0)
        block += QStringLiteral("-%1ch").arg(channels);
    if (samplerate > 0)
        block += QStringLiteral("-%1Hz").arg(samplerate);

    return cleaned + block + extension;
}

} // binary_name_hints
} // data
} // pv

#endif // PXVIEW_PV_DATA_BINARY_NAME_HINTS_H
