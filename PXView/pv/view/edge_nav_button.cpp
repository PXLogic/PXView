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

#include "edge_nav_button.h"

#include "../config/appconfig.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

namespace pv {
namespace view {

static constexpr int ButtonSize = 24;
static constexpr int BorderRadius = 3;

EdgeNavButton::EdgeNavButton(Direction dir, QWidget *parent)
    : QWidget(parent), _dir(dir), _hovered(false) {
  setFixedSize(ButtonSize, ButtonSize);
  setCursor(Qt::PointingHandCursor);
  setAttribute(Qt::WA_TransparentForMouseEvents, false);
  UpdateTheme();
}

void EdgeNavButton::UpdateTheme() {
  _bgColor = AppConfig::Instance().GetThemeColor("@panel-bg");
  if (!_bgColor.isValid())
    _bgColor = QColor("#1a1a1a");

  _borderColor = AppConfig::Instance().GetThemeColor("@border-strong");
  if (!_borderColor.isValid())
    _borderColor = QColor("#555555");

  _arrowColor = AppConfig::Instance().GetThemeColor("@panel-text");
  if (!_arrowColor.isValid())
    _arrowColor = QColor("#f5f0e5");

  _disabledColor = QColor(_arrowColor.red(), _arrowColor.green(),
                          _arrowColor.blue(), 60);

  update();
}

void EdgeNavButton::paintEvent(QPaintEvent *event) {
  (void)event;
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  // Background
  QColor bg = _bgColor;
  if (_hovered && isEnabled()) {
    bg = bg.lighter(150);
  }
  p.setPen(QPen(isEnabled() ? _borderColor : _disabledColor, 1));
  p.setBrush(bg);
  p.drawRoundedRect(rect().adjusted(0.5, 0.5, -0.5, -0.5), BorderRadius,
                    BorderRadius);

  // Arrow
  QColor arrowCol = isEnabled() ? _arrowColor : _disabledColor;
  p.setPen(Qt::NoPen);
  p.setBrush(arrowCol);

  const int cx = width() / 2;
  const int cy = height() / 2;
  const int aw = 5; // arrow half-width
  const int ah = 6; // arrow half-height

  QPainterPath arrow;
  if (_dir == Previous) {
    // Left-pointing triangle
    arrow.moveTo(cx - aw + 1, cy);
    arrow.lineTo(cx + aw - 1, cy - ah);
    arrow.lineTo(cx + aw - 1, cy + ah);
    arrow.closeSubpath();
  } else {
    // Right-pointing triangle
    arrow.moveTo(cx + aw - 1, cy);
    arrow.lineTo(cx - aw + 1, cy - ah);
    arrow.lineTo(cx - aw + 1, cy + ah);
    arrow.closeSubpath();
  }
  p.drawPath(arrow);
}

void EdgeNavButton::enterEvent(QEnterEvent *event) {
  (void)event;
  _hovered = true;
  update();
}

void EdgeNavButton::leaveEvent(QEvent *event) {
  (void)event;
  _hovered = false;
  update();
}

void EdgeNavButton::mousePressEvent(QMouseEvent *event) {
  (void)event;
  if (isEnabled())
    emit clicked();
}

} // namespace view
} // namespace pv
