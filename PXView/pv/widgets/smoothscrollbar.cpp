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

#include "smoothscrollbar.h"

#include <QWheelEvent>

namespace pv {
namespace widgets {

SmoothScrollBar::SmoothScrollBar(Qt::Orientation orientation, QWidget *parent)
    : QScrollBar(orientation, parent), _anim_duration(300),
      _slider_dragging(false), _wheel_dir(0), _wheel_count(0), _anim_target(0) {
  _anim = new QPropertyAnimation(this, "value", this);
  _anim->setEasingCurve(QEasingCurve(QEasingCurve::OutCubic));
  _anim->setDuration(_anim_duration);

  _accel_timer.setInterval(100);
  _accel_timer.setSingleShot(true);
  connect(&_accel_timer, &QTimer::timeout, this,
          &SmoothScrollBar::onAccelerationTimeout);

  connect(this, &QAbstractSlider::sliderPressed, this, [this]() {
    _slider_dragging = true;
    _anim->stop();
  });
  connect(this, &QAbstractSlider::sliderReleased, this,
          [this]() { _slider_dragging = false; });
}

SmoothScrollBar::~SmoothScrollBar() {}

void SmoothScrollBar::smoothSetValue(int value) {
  if (_slider_dragging) {
    QScrollBar::setValue(value);
    return;
  }

  value = qBound(minimum(), value, maximum());
  if (value == this->value() && _anim->state() != QAbstractAnimation::Running)
    return;

  int curVal = this->value();

  if (_anim->state() == QAbstractAnimation::Running) {
    _anim_target += (value - curVal);
    _anim_target = qBound((qreal)minimum(), _anim_target, (qreal)maximum());
  } else {
    _anim_target = value;
  }

  int targetInt = qRound(_anim_target);

  _anim->stop();
  curVal = this->value();
  _anim->setStartValue(curVal);
  _anim->setEndValue(targetInt);
  _anim->setDuration(_anim_duration);
  _anim->setEasingCurve(QEasingCurve(QEasingCurve::OutCubic));
  _anim->start();
}

void SmoothScrollBar::immediateSetValue(int value) {
  _anim->stop();
  QScrollBar::setValue(value);
}

void SmoothScrollBar::setAnimationDuration(int ms) { _anim_duration = ms; }

int SmoothScrollBar::animationDuration() const { return _anim_duration; }

void SmoothScrollBar::wheelEvent(QWheelEvent *event) {
  event->accept();

  int delta = event->angleDelta().y();
  if (delta == 0)
    return;

  int direction = (delta > 0) ? -1 : 1;

  if (_accel_timer.isActive() && _wheel_dir == direction) {
    _wheel_count++;
  } else {
    _wheel_count = 1;
    _wheel_dir = direction;
  }

  int step = BASE_STEP;
  int duration = 3 * FIXUP_DURATION / 4;

  if (_wheel_count > 6) {
    step = BASE_STEP * 5;
    duration = 5000;
  } else if (_wheel_count > 3) {
    step = BASE_STEP * 2;
    duration = 800;
  }

  int curVal = value();

  if (_anim->state() == QAbstractAnimation::Running && _wheel_count > 1) {
    _anim_target += direction * step;
  } else {
    _anim_target = curVal + direction * step;
  }

  _anim_target = qBound((qreal)minimum(), _anim_target, (qreal)maximum());

  int targetInt = qRound(_anim_target);
  if (targetInt == curVal) {
    _accel_timer.start();
    return;
  }

  _anim->stop();
  curVal = value();
  _anim->setStartValue(curVal);
  _anim->setEndValue(targetInt);
  _anim->setDuration(duration);
  _anim->setEasingCurve(QEasingCurve(QEasingCurve::OutCubic));
  _anim->start();

  _accel_timer.start();
}

void SmoothScrollBar::onAccelerationTimeout() {
  _wheel_count = 0;
  _wheel_dir = 0;
}

} // namespace widgets
} // namespace pv
