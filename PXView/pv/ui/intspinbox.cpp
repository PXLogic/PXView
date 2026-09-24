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

#include "pv/ui/intspinbox.h"

#include <QLineEdit>
#include <QSignalBlocker>

#include <algorithm>

namespace pv {
namespace ui {

IntSpinBox::IntSpinBox(QWidget *parent) :
    QAbstractSpinBox(parent)
{
    setAlignment(Qt::AlignRight);
    // No per-keystroke valueChanged: a half-typed number must not reach the
    // device (the value is taken on editingFinished / interpretText()).
    setKeyboardTracking(false);

    connect(lineEdit(), &QLineEdit::editingFinished,
            this, &IntSpinBox::on_editing_finished);
    updateText();
}

void IntSpinBox::setUnsignedRange(uint64_t minimum, uint64_t maximum)
{
    _unsigned_minimum = minimum;
    _unsigned_maximum = std::max(minimum, maximum);
    if (_unsigned_value < _unsigned_minimum)
        setUnsignedValue(_unsigned_minimum);
    else if (_unsigned_value > _unsigned_maximum)
        setUnsignedValue(_unsigned_maximum);
}

void IntSpinBox::setSignedRange(int64_t minimum, int64_t maximum)
{
    _signed_minimum = minimum;
    _signed_maximum = std::max(minimum, maximum);
    if (_signed_value < _signed_minimum)
        setSignedValue(_signed_minimum);
    else if (_signed_value > _signed_maximum)
        setSignedValue(_signed_maximum);
}

void IntSpinBox::setUnsignedMinimum(uint64_t minimum)
{
    setUnsignedRange(minimum, _unsigned_maximum);
}

void IntSpinBox::setUnsignedMaximum(uint64_t maximum)
{
    setUnsignedRange(_unsigned_minimum, maximum);
}

void IntSpinBox::setUnsignedValue(uint64_t value)
{
    const uint64_t clamped = std::min(std::max(value, _unsigned_minimum),
                                      _unsigned_maximum);
    if (clamped == _unsigned_value)
        return;

    _unsigned_value = clamped;
    _signed_value = static_cast<int64_t>(clamped);
    updateText();
    emit valueChanged();
}

void IntSpinBox::setSignedValue(int64_t value)
{
    const int64_t clamped = std::min(std::max(value, _signed_minimum),
                                     _signed_maximum);
    if (clamped == _signed_value)
        return;

    _signed_value = clamped;
    _unsigned_value = static_cast<uint64_t>(clamped);
    updateText();
    emit valueChanged();
}

void IntSpinBox::setSingleStep(uint64_t step)
{
    _single_step = step ? step : 1;
}

void IntSpinBox::setSuffix(const QString &suffix)
{
    _suffix = suffix;
    updateText();
}

QString IntSpinBox::valueToText() const
{
    const QString number = _signed ? QString::number(_signed_value)
                                   : QString::number(_unsigned_value);
    return number + _suffix;
}

bool IntSpinBox::parseText(const QString &text, uint64_t *unsigned_out,
                           int64_t *signed_out) const
{
    QString trimmed = text.trimmed();
    // The suffix is part of the field's text ("1000000Hz"): the number is what
    // precedes it.
    if (!_suffix.isEmpty() && trimmed.endsWith(_suffix))
        trimmed.chop(_suffix.length());
    trimmed = trimmed.trimmed();
    if (trimmed.isEmpty())
        return false;

    // "-" alone is not a number (validate() reports it as still-in-progress).
    if (trimmed == QLatin1String("-"))
        return false;

    if (_signed) {
        bool ok = false;
        const int64_t value = trimmed.toLongLong(&ok);
        if (!ok || value < _signed_minimum || value > _signed_maximum)
            return false;
        *signed_out = value;
        *unsigned_out = static_cast<uint64_t>(value);
        return true;
    }

    // Digit-only: toULongLong() would also accept a leading '-'/'+'.
    for (int i = 0; i < trimmed.size(); i++) {
        if (!trimmed.at(i).isDigit())
            return false;
    }

    bool ok = false;
    const uint64_t value = trimmed.toULongLong(&ok);
    if (!ok || value < _unsigned_minimum || value > _unsigned_maximum)
        return false;

    *unsigned_out = value;
    *signed_out = static_cast<int64_t>(value);
    return true;
}

void IntSpinBox::updateText()
{
    if (!lineEdit())
        return;

    const QSignalBlocker blocker(lineEdit());
    const int cursor = lineEdit()->cursorPosition();
    lineEdit()->setText(valueToText());
    lineEdit()->setCursorPosition(cursor);
}

void IntSpinBox::stepBy(int steps)
{
    if (steps == 0)
        return;

    // One step at a time with saturation: a spin box may be nudged by a wheel
    // with a larger step count, and the arithmetic must not wrap around.
    for (int i = 0; i < qAbs(steps); i++) {
        if (_signed) {
            if (steps > 0) {
                if (_signed_value > _signed_maximum - static_cast<int64_t>(_single_step)) {
                    setSignedValue(_signed_maximum);
                    break;
                }
                setSignedValue(_signed_value + static_cast<int64_t>(_single_step));
            } else {
                if (_signed_value < _signed_minimum + static_cast<int64_t>(_single_step)) {
                    setSignedValue(_signed_minimum);
                    break;
                }
                setSignedValue(_signed_value - static_cast<int64_t>(_single_step));
            }
        } else {
            if (steps > 0) {
                if (_unsigned_value > _unsigned_maximum - _single_step) {
                    setUnsignedValue(_unsigned_maximum);
                    break;
                }
                setUnsignedValue(_unsigned_value + _single_step);
            } else {
                if (_unsigned_value < _unsigned_minimum + _single_step) {
                    setUnsignedValue(_unsigned_minimum);
                    break;
                }
                setUnsignedValue(_unsigned_value - _single_step);
            }
        }
    }
}

QAbstractSpinBox::StepEnabled IntSpinBox::stepEnabled() const
{
    StepEnabled enabled = StepNone;

    if (_signed) {
        if (_signed_value < _signed_maximum)
            enabled |= StepUpEnabled;
        if (_signed_value > _signed_minimum)
            enabled |= StepDownEnabled;
    } else {
        if (_unsigned_value < _unsigned_maximum)
            enabled |= StepUpEnabled;
        if (_unsigned_value > _unsigned_minimum)
            enabled |= StepDownEnabled;
    }

    return enabled;
}

QValidator::State IntSpinBox::validate(QString &input, int &pos) const
{
    Q_UNUSED(pos);

    QString text = input.trimmed();
    if (!_suffix.isEmpty() && text.endsWith(_suffix))
        text.chop(_suffix.length());
    text = text.trimmed();

    if (text.isEmpty())
        return QValidator::Intermediate;  // the user is still typing

    // Permit a lone '-' while a negative number is being typed, and reject it
    // outright for unsigned boxes.
    if (text.at(0) == QLatin1Char('-')) {
        if (!_signed)
            return QValidator::Invalid;
        if (text.size() == 1)
            return QValidator::Intermediate;
    }

    uint64_t unsigned_value = 0;
    int64_t signed_value = 0;
    if (!parseText(input, &unsigned_value, &signed_value))
        return QValidator::Invalid;

    return QValidator::Acceptable;
}

void IntSpinBox::fixup(QString &input) const
{
    uint64_t unsigned_value = 0;
    int64_t signed_value = 0;
    if (parseText(input, &unsigned_value, &signed_value))
        return;  // nothing to repair

    // Out of range or not a number at all: fall back to the current value so the
    // field never keeps something the module would reject.
    input = valueToText();
}

void IntSpinBox::on_editing_finished()
{
    if (!lineEdit())
        return;

    QString text = lineEdit()->text();
    int pos = 0;
    if (validate(text, pos) != QValidator::Acceptable) {
        // Out of range or not a number: repair the text before taking the value
        // (Qt calls interpretText() for Key_Enter, but not on focus loss).
        fixup(text);
        const QSignalBlocker blocker(lineEdit());
        lineEdit()->setText(text);
    }

    uint64_t unsigned_value = 0;
    int64_t signed_value = 0;
    if (!parseText(text, &unsigned_value, &signed_value))
        return;

    if (_signed)
        setSignedValue(signed_value);
    else
        setUnsignedValue(unsigned_value);
}

} // ui
} // pv
