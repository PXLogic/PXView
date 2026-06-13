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

#include "smoothtablehelper.h"

#include <QHeaderView>
#include <QScrollBar>
#include <QTableView>
#include <QWheelEvent>

namespace pv {
namespace widgets {

SmoothTableHelper::SmoothTableHelper(QTableView *tableView, QObject *parent)
    : QObject(parent), _table(tableView), _v_target(0), _h_target(0),
      _v_wheel_count(0), _h_wheel_count(0), _v_wheel_dir(0), _h_wheel_dir(0),
      _v_fixing_up(false) {
  _v_anim = new QPropertyAnimation(_table->verticalScrollBar(), "value", this);
  _h_anim =
      new QPropertyAnimation(_table->horizontalScrollBar(), "value", this);

  connect(_v_anim, &QPropertyAnimation::finished, this,
          &SmoothTableHelper::onVAnimFinished);
  connect(_h_anim, &QPropertyAnimation::finished, this,
          &SmoothTableHelper::onHAnimFinished);

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

  connect(_table->verticalScrollBar(), &QAbstractSlider::sliderPressed, this,
          [this]() { _v_anim->stop(); });
  connect(_table->horizontalScrollBar(), &QAbstractSlider::sliderPressed, this,
          [this]() { _h_anim->stop(); });

  _table->viewport()->installEventFilter(this);
}

SmoothTableHelper::~SmoothTableHelper() {}

bool SmoothTableHelper::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::Wheel) {
    QWheelEvent *wheelEvent = static_cast<QWheelEvent *>(event);
    int deltaY = wheelEvent->angleDelta().y();
    int deltaX = wheelEvent->angleDelta().x();

    if (deltaY != 0)
      handleVWheel(deltaY);
    if (deltaX != 0)
      handleHWheel(deltaX);

    wheelEvent->accept();
    return true;
  }
  return QObject::eventFilter(watched, event);
}

void SmoothTableHelper::handleVWheel(int delta) {
  _v_fixing_up = false;

  int direction = (delta > 0) ? -1 : 1;

  if (_v_accel_timer.isActive() && _v_wheel_dir == direction) {
    _v_wheel_count++;
  } else {
    _v_wheel_count = 1;
    _v_wheel_dir = direction;
  }

  int rowHeight = _table->verticalHeader()->defaultSectionSize();
  int step = qMax<int>(BASE_V_STEP, rowHeight * 3);
  int duration = 300;
  QEasingCurve easing(QEasingCurve::OutExpo);

  if (_v_wheel_count > 6) {
    step = qMax<int>(BASE_V_STEP * 5, rowHeight * 15);
    duration = 5000;
    easing.setType(QEasingCurve::OutCubic);
  } else if (_v_wheel_count > 3) {
    step = qMax<int>(BASE_V_STEP * 2, rowHeight * 6);
    duration = 800;
    easing.setType(QEasingCurve::OutCubic);
  }

  QScrollBar *vbar = _table->verticalScrollBar();
  int vmin = vbar->minimum();
  int vmax = vbar->maximum();

  if (vmin >= vmax)
    return;

  int curVal = vbar->value();

  if (_v_anim->state() == QAbstractAnimation::Running && _v_wheel_count > 1) {
    _v_target += direction * step;
  } else {
    _v_target = curVal + direction * step;
  }

  if (rowHeight > 0) {
    _v_target = qRound(_v_target / (qreal)rowHeight) * rowHeight;
  }

  _v_target = qBound((qreal)vmin, _v_target, (qreal)vmax);

  int targetInt = qRound(_v_target);
  if (targetInt == curVal) {
    _v_accel_timer.start();
    return;
  }

  _v_anim->stop();
  curVal = vbar->value();
  _v_anim->setStartValue(curVal);
  _v_anim->setEndValue(targetInt);
  _v_anim->setDuration(duration);
  _v_anim->setEasingCurve(easing);
  _v_anim->start();

  _v_accel_timer.start();
}

void SmoothTableHelper::handleHWheel(int delta) {
  int direction = (delta > 0) ? -1 : 1;

  if (_h_accel_timer.isActive() && _h_wheel_dir == direction) {
    _h_wheel_count++;
  } else {
    _h_wheel_count = 1;
    _h_wheel_dir = direction;
  }

  int step = BASE_H_STEP;
  int duration = 300;
  QEasingCurve easing(QEasingCurve::OutExpo);

  if (_h_wheel_count > 6) {
    step = BASE_H_STEP * 5;
    duration = 5000;
    easing.setType(QEasingCurve::OutCubic);
  } else if (_h_wheel_count > 3) {
    step = BASE_H_STEP * 2;
    duration = 800;
    easing.setType(QEasingCurve::OutCubic);
  }

  QScrollBar *hbar = _table->horizontalScrollBar();
  int hmin = hbar->minimum();
  int hmax = hbar->maximum();

  if (hmin >= hmax)
    return;

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
  _h_anim->setStartValue(curVal);
  _h_anim->setEndValue(targetInt);
  _h_anim->setDuration(duration);
  _h_anim->setEasingCurve(easing);
  _h_anim->start();

  _h_accel_timer.start();
}

void SmoothTableHelper::onVAnimFinished() {
  if (_v_fixing_up) {
    _v_fixing_up = false;
    return;
  }

  QScrollBar *vbar = _table->verticalScrollBar();
  int rowHeight = _table->verticalHeader()->defaultSectionSize();
  if (rowHeight <= 0)
    return;

  int currentValue = vbar->value();
  int nearestRow = qRound((qreal)currentValue / rowHeight);
  int snapTarget = nearestRow * rowHeight;

  if (snapTarget != currentValue && snapTarget >= vbar->minimum() &&
      snapTarget <= vbar->maximum()) {
    _v_fixing_up = true;
    int scrollPixel = snapTarget - currentValue;
    _v_anim->stop();
    _v_anim->setStartValue(currentValue);
    _v_anim->setEndValue(currentValue + scrollPixel);
    _v_anim->setDuration(150);
    _v_anim->setEasingCurve(QEasingCurve(QEasingCurve::OutExpo));
    _v_anim->start();
  }
}

void SmoothTableHelper::onHAnimFinished() {}

} // namespace widgets
} // namespace pv
