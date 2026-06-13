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

#ifndef PXVIEW_PV_WIDGETS_SEARCHPATTERNINPUT_H
#define PXVIEW_PV_WIDGETS_SEARCHPATTERNINPUT_H

#include <QWidget>
#include <map>
#include <stdint.h>
#include <QVector>

namespace pv {
namespace widgets {

class SearchPatternInput : public QWidget
{
    Q_OBJECT

public:
    explicit SearchPatternInput(QWidget *parent = nullptr);

    void set_channel_count(int count);
    int channel_count() const;

    std::map<uint16_t, QString> get_pattern() const;
    void set_pattern(const std::map<uint16_t, QString> &pattern);

    QSize sizeHint() const override;

signals:
    void pattern_changed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    int cellWidth() const;
    int charIndexAt(int x) const;

    int _channel_count;
    int _cursor_pos;
    QVector<QChar> _chars;
    bool _has_focus;

    static const int kCellWidth = 18;
    static const int kCellHeight = 22;
    static const int kLabelHeight = 14;
    static const int kBorderRadius = 3;
    static const int kPadding = 1;
};

} // namespace widgets
} // namespace pv

#endif // PXVIEW_PV_WIDGETS_SEARCHPATTERNINPUT_H
