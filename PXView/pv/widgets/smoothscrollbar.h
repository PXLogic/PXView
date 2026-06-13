/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
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
 */

#ifndef PXVIEW_PV_WIDGETS_SMOOTHSCROLLBAR_H
#define PXVIEW_PV_WIDGETS_SMOOTHSCROLLBAR_H

#include <QScrollBar>
#include <QPropertyAnimation>
#include <QTimer>

namespace pv {
namespace widgets {

class SmoothScrollBar : public QScrollBar
{
    Q_OBJECT

public:
    explicit SmoothScrollBar(Qt::Orientation orientation, QWidget *parent = nullptr);
    ~SmoothScrollBar();

    void smoothSetValue(int value);
    void immediateSetValue(int value);

    void setAnimationDuration(int ms);
    int animationDuration() const;

protected:
    void wheelEvent(QWheelEvent *event) override;

private:
    void onAccelerationTimeout();

    QPropertyAnimation *_anim;
    int _anim_duration;
    bool _slider_dragging;

    int _wheel_dir;
    int _wheel_count;
    qreal _anim_target;
    QTimer _accel_timer;

    enum { FIXUP_DURATION = 400, BASE_STEP = 72 };
};

} // namespace widgets
} // namespace pv

#endif // PXVIEW_PV_WIDGETS_SMOOTHSCROLLBAR_H
