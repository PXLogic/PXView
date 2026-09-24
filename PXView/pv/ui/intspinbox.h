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

#ifndef PXVIEW_PV_UI_INTSPINBOX_H
#define PXVIEW_PV_UI_INTSPINBOX_H

#include <QAbstractSpinBox>

#include <cstdint>

namespace pv {
namespace ui {

/**
 * An integer spin box covering the full 64-bit range.
 *
 * QSpinBox stores its value in an int, so it cannot represent what libsigrok
 * options declare: a 4 GHz samplerate is beyond INT_MAX, and VCD's "skip"
 * defaults to UINT64_MAX ("start at the first timestamp"). PXView's Int used to
 * clamp such values for display and — worse — submitted the clamp as soon as the
 * import dialog was merely accepted, which silently changed the meaning of the
 * option.
 *
 * QAbstractSpinBox keeps the value as int64_t/uint64_t, so nothing is lost on
 * the way (unlike QDoubleSpinBox, which is limited to 2^53); only the text
 * conversion goes through Qt. This is the "custom widget" the @todo in
 * pv/prop/int.cpp asks for; it follows the reference PulseView's
 * pv/widgets/timestampspinbox.cpp, which derives from QAbstractSpinBox the same
 * way.
 *
 * Signedness is explicit (setSigned()): signed boxes accept a leading '-' and
 * use signed bounds, unsigned boxes accept digits only.
 */
class IntSpinBox : public QAbstractSpinBox
{
    Q_OBJECT

    // Designer/QUiLoader integration, mirroring TimestampSpinBox: "promote to"
    // can drive value/minimum/maximum from a .ui file.
    Q_PROPERTY(qulonglong value READ unsignedValue WRITE setUnsignedValue
                   NOTIFY valueChanged USER true)
    Q_PROPERTY(qulonglong minimum READ unsignedMinimum WRITE setUnsignedMinimum)
    Q_PROPERTY(qulonglong maximum READ unsignedMaximum WRITE setUnsignedMaximum)

public:
    explicit IntSpinBox(QWidget *parent = nullptr);

    void setSigned(bool is_signed) { _signed = is_signed; }
    bool isSigned() const { return _signed; }

    void setUnsignedRange(uint64_t minimum, uint64_t maximum);
    void setSignedRange(int64_t minimum, int64_t maximum);

    uint64_t unsignedMinimum() const { return _unsigned_minimum; }
    uint64_t unsignedMaximum() const { return _unsigned_maximum; }
    void setUnsignedMinimum(uint64_t minimum);
    void setUnsignedMaximum(uint64_t maximum);

    int64_t signedMinimum() const { return _signed_minimum; }
    int64_t signedMaximum() const { return _signed_maximum; }

    uint64_t unsignedValue() const { return _unsigned_value; }
    int64_t signedValue() const { return _signed_value; }
    void setUnsignedValue(uint64_t value);
    void setSignedValue(int64_t value);

    uint64_t singleStep() const { return _single_step; }
    void setSingleStep(uint64_t step);

    /* QAbstractSpinBox has no suffix support (only QSpinBox/QDoubleSpinBox do),
     * so the field's text carries it explicitly: "<number><suffix>". */
    void setSuffix(const QString &suffix);
    const QString &suffix() const { return _suffix; }

    void stepBy(int steps) override;
    StepEnabled stepEnabled() const override;
    QValidator::State validate(QString &input, int &pos) const override;
    void fixup(QString &input) const override;

signals:
    void valueChanged();

private slots:
    /* editingFinished can arrive before the text is interpreted (Qt only calls
     * interpretText() for Key_Enter, not on focus loss), so the value is parsed
     * here as well. */
    void on_editing_finished();

private:
    QString valueToText() const;
    /* Parse @p text as the active signedness and check it against the bounds.
     * Used by validate(), fixup() and on_editing_finished(), so all three agree. */
    bool parseText(const QString &text, uint64_t *unsigned_out,
                   int64_t *signed_out) const;
    void updateText();

    bool _signed = false;

    uint64_t _unsigned_minimum = 0;
    uint64_t _unsigned_maximum = UINT64_MAX;
    uint64_t _unsigned_value = 0;

    int64_t _signed_minimum = INT64_MIN;
    int64_t _signed_maximum = INT64_MAX;
    int64_t _signed_value = 0;

    uint64_t _single_step = 1;

    QString _suffix;
};

} // ui
} // pv

#endif // PXVIEW_PV_UI_INTSPINBOX_H
