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

#ifndef PXVIEW_PV_WIDGETS_SMOOTHSCROLLAREA_H
#define PXVIEW_PV_WIDGETS_SMOOTHSCROLLAREA_H

#include <QPropertyAnimation>
#include <QScrollArea>
#include <QTimer>

namespace pv {
namespace widgets {

class SmoothScrollArea : public QScrollArea {
  Q_OBJECT

public:
  explicit SmoothScrollArea(QWidget *parent = nullptr);
  ~SmoothScrollArea();

  void setLongTailAnimation(bool enabled);

protected:
  void wheelEvent(QWheelEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void handleVWheel(int delta);
  void handleHWheel(int delta);

  QPropertyAnimation *_v_anim;
  QPropertyAnimation *_h_anim;
  qreal _v_target;
  qreal _h_target;

  QTimer _v_accel_timer;
  QTimer _h_accel_timer;
  int _v_wheel_count;
  int _h_wheel_count;
  int _v_wheel_dir;
  int _h_wheel_dir;

  bool _long_tail;

  enum { BASE_STEP = 72 };
};

} // namespace widgets
} // namespace pv

#endif // PXVIEW_PV_WIDGETS_SMOOTHSCROLLAREA_H
