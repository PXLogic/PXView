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

#include "smoothscrollarea.h"

#include <QLayout>
#include <QScrollBar>
#include <QWheelEvent>

namespace pv {
namespace widgets {

SmoothScrollArea::SmoothScrollArea(QWidget *parent)
    : QScrollArea(parent), _v_target(0), _h_target(0), _v_wheel_count(0),
      _h_wheel_count(0), _v_wheel_dir(0), _h_wheel_dir(0), _long_tail(false) {
  _v_anim = new QPropertyAnimation(verticalScrollBar(), "value", this);
  _h_anim = new QPropertyAnimation(horizontalScrollBar(), "value", this);

  _v_accel_timer.setInterval(100);
  _v_accel_timer.setSingleShot(true);
  connect(&_v_accel_timer, &QTimer::timeout, this, [this]() {
    _v_wheel_count = 0;
    _v_wheel_dir = 0;
  });

  _h_accel_timer.setInterval(100);
  _h_accel_timer.setSingleShot(true);
  connect(&_h_accel_timer, &QTimer::timeout, this, [this]() {
    _h_wheel_count = 0;
    _h_wheel_dir = 0;
  });

  connect(verticalScrollBar(), &QAbstractSlider::sliderPressed, this,
          [this]() { _v_anim->stop(); });
  connect(horizontalScrollBar(), &QAbstractSlider::sliderPressed, this,
          [this]() { _h_anim->stop(); });

  viewport()->installEventFilter(this);
}

SmoothScrollArea::~SmoothScrollArea() {}

void SmoothScrollArea::setLongTailAnimation(bool enabled) {
  _long_tail = enabled;
}

bool SmoothScrollArea::eventFilter(QObject *watched, QEvent *event) {
  if (watched == viewport() && event->type() == QEvent::Wheel) {
    wheelEvent(static_cast<QWheelEvent *>(event));
    return true;
  }
  return QScrollArea::eventFilter(watched, event);
}

void SmoothScrollArea::wheelEvent(QWheelEvent *event) {
  int deltaY = event->angleDelta().y();
  int deltaX = event->angleDelta().x();

  if (deltaY != 0)
    handleVWheel(deltaY);
  if (deltaX != 0)
    handleHWheel(deltaX);

  event->accept();
}

void SmoothScrollArea::handleVWheel(int delta) {
  int direction = (delta > 0) ? -1 : 1;

  if (_v_accel_timer.isActive() && _v_wheel_dir == direction) {
    _v_wheel_count++;
  } else {
    _v_wheel_count = 1;
    _v_wheel_dir = direction;
  }

  int step = BASE_STEP;
  int duration = 300;
  QEasingCurve easing(QEasingCurve::OutExpo);

  QScrollBar *vbar = verticalScrollBar();
  int vmin = vbar->minimum();
  int vmax = vbar->maximum();

  if (vmin >= vmax)
    return;

  if (_v_wheel_count > 6) {
    step = BASE_STEP * 5;
    if (_long_tail) {
      duration = 5000;
      easing.setType(QEasingCurve::OutCubic);
    } else {
      duration = qBound(300, qAbs(qRound(_v_target) - vbar->value()) * 3, 1200);
    }
  } else if (_v_wheel_count > 3) {
    step = BASE_STEP * 2;
    if (_long_tail) {
      duration = 800;
      easing.setType(QEasingCurve::OutCubic);
    } else {
      duration = qBound(300, qAbs(qRound(_v_target) - vbar->value()) * 2, 600);
    }
  }

  int curVal = vbar->value();

  if (_v_anim->state() == QAbstractAnimation::Running && _v_wheel_count > 1) {
    _v_target += direction * step;
  } else {
    _v_target = curVal + direction * step;
  }

  _v_target = qBound((qreal)vmin, _v_target, (qreal)vmax);

  int targetInt = qRound(_v_target);
  if (targetInt == curVal) {
    _v_accel_timer.start();
    return;
  }

  _v_anim->stop();
  curVal = vbar->value();

  if (widget())
    widget()->setAttribute(Qt::WA_TransparentForMouseEvents, true);

  _v_anim->setStartValue(curVal);
  _v_anim->setEndValue(targetInt);
  _v_anim->setDuration(duration);
  _v_anim->setEasingCurve(easing);

  disconnect(_v_anim, &QPropertyAnimation::finished, nullptr, nullptr);
  connect(_v_anim, &QPropertyAnimation::finished, this, [this]() {
    if (widget() && widget()->layout()) {
      widget()->layout()->setEnabled(true);
      widget()->layout()->activate();
    }
    if (widget())
      widget()->setAttribute(Qt::WA_TransparentForMouseEvents, false);
  });

  if (widget() && widget()->layout())
    widget()->layout()->setEnabled(false);

  _v_anim->start();

  _v_accel_timer.start();
}

void SmoothScrollArea::handleHWheel(int delta) {
  int direction = (delta > 0) ? -1 : 1;

  if (_h_accel_timer.isActive() && _h_wheel_dir == direction) {
    _h_wheel_count++;
  } else {
    _h_wheel_count = 1;
    _h_wheel_dir = direction;
  }

  int step = BASE_STEP;
  int duration = 300;
  QEasingCurve easing(QEasingCurve::OutExpo);

  QScrollBar *hbar = horizontalScrollBar();
  int hmin = hbar->minimum();
  int hmax = hbar->maximum();

  if (hmin >= hmax)
    return;

  if (_h_wheel_count > 6) {
    step = BASE_STEP * 5;
    if (_long_tail) {
      duration = 5000;
      easing.setType(QEasingCurve::OutCubic);
    } else {
      duration = qBound(300, qAbs(qRound(_h_target) - hbar->value()) * 3, 1200);
    }
  } else if (_h_wheel_count > 3) {
    step = BASE_STEP * 2;
    if (_long_tail) {
      duration = 800;
      easing.setType(QEasingCurve::OutCubic);
    } else {
      duration = qBound(300, qAbs(qRound(_h_target) - hbar->value()) * 2, 600);
    }
  }

  int curVal = hbar->value();

  if (_h_anim->state() == QAbstractAnimation::Running && _h_wheel_count > 1) {
    _h_target += direction * step;
  } else {
    _h_target = curVal + direction * step;
  }

  _h_target = qBound((qreal)hmin, _h_target, (qreal)hmax);

  int targetInt = qRound(_h_target);
  if (targetInt == curVal) {
    _h_accel_timer.start();
    return;
  }

  _h_anim->stop();
  curVal = hbar->value();

  if (widget())
    widget()->setAttribute(Qt::WA_TransparentForMouseEvents, true);

  _h_anim->setStartValue(curVal);
  _h_anim->setEndValue(targetInt);
  _h_anim->setDuration(duration);
  _h_anim->setEasingCurve(easing);

  disconnect(_h_anim, &QPropertyAnimation::finished, nullptr, nullptr);
  connect(_h_anim, &QPropertyAnimation::finished, this, [this]() {
    if (widget() && widget()->layout()) {
      widget()->layout()->setEnabled(true);
      widget()->layout()->activate();
    }
    if (widget())
      widget()->setAttribute(Qt::WA_TransparentForMouseEvents, false);
  });

  if (widget() && widget()->layout())
    widget()->layout()->setEnabled(false);

  _h_anim->start();

  _h_accel_timer.start();
}

} // namespace widgets
} // namespace pv
