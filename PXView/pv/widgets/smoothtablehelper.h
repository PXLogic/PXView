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

#ifndef PXVIEW_PV_WIDGETS_SMOOTHTABLEHELPER_H
#define PXVIEW_PV_WIDGETS_SMOOTHTABLEHELPER_H

#include <QObject>
#include <QPropertyAnimation>
#include <QTimer>

class QTableView;

namespace pv {
namespace widgets {

class SmoothTableHelper : public QObject {
  Q_OBJECT

public:
  explicit SmoothTableHelper(QTableView *tableView, QObject *parent = nullptr);
  ~SmoothTableHelper();

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void handleVWheel(int delta);
  void handleHWheel(int delta);
  void onVAnimFinished();
  void onHAnimFinished();

  QTableView *_table;
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

  bool _v_fixing_up;

  enum { FIXUP_DURATION = 400, BASE_V_STEP = 72, BASE_H_STEP = 72 };
};

} // namespace widgets
} // namespace pv

#endif // PXVIEW_PV_WIDGETS_SMOOTHTABLEHELPER_H
