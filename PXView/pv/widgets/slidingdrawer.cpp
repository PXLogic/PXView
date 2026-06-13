/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "slidingdrawer.h"

#include <QApplication>
#include <QTimer>
#ifndef NDEBUG
#include <QElapsedTimer>
#endif
#include <QEasingCurve>
#include <QLabel>
#include <QVBoxLayout>
#ifndef NDEBUG
#include "../log.h"
#endif

namespace pv {
namespace widgets {

static constexpr int DEFAULT_DRAWER_WIDTH = 350;
static constexpr int DEFAULT_ANIMATION_DURATION = 500;
static constexpr int TITLE_BAR_HEIGHT = 40;
static constexpr int MIN_DRAWER_WIDTH = 200;

static qreal cssBezierEasing(qreal t) {
  static const double x1 = 0.4, y1 = 0.0;
  static const double x2 = 0.2, y2 = 1.0;

  double cx = 3.0 * x1;
  double bx = 3.0 * (x2 - x1) - cx;
  double ax = 1.0 - cx - bx;

  double cy = 3.0 * y1;
  double by = 3.0 * (y2 - y1) - cy;
  double ay = 1.0 - cy - by;

  auto sampleCurveX = [&](double s) { return ((ax * s + bx) * s + cx) * s; };
  auto sampleCurveY = [&](double s) { return ((ay * s + by) * s + cy) * s; };

  double s = t;
  for (int i = 0; i < 8; i++) {
    double err = sampleCurveX(s) - t;
    if (qAbs(err) < 1e-7)
      break;
    double deriv = (3.0 * ax * s + 2.0 * bx) * s + cx;
    if (qAbs(deriv) < 1e-10)
      break;
    s -= err / deriv;
  }
  return qBound(0.0, sampleCurveY(s), 1.0);
}

static QEasingCurve makeTailwindCurve() {
  QEasingCurve curve;
  curve.setCustomType(cssBezierEasing);
  return curve;
}

// ============================================================================

SlidingDrawer::SlidingDrawer(QWidget *parent)
    : QWidget(parent), _stacked_widget(nullptr), _panel_content(nullptr),
      _title_bar(nullptr), _title_label(nullptr), _push_layout(nullptr),
      _animation(nullptr),
      _slide_offset(DEFAULT_DRAWER_WIDTH) // fully hidden initially
      ,
      _drawer_width(DEFAULT_DRAWER_WIDTH),
      _animation_duration(DEFAULT_ANIMATION_DURATION), _current_page(-1),
      _is_open(false), _is_animating(false), _drag_active(false),
      _drag_start_drawer_width(0), _drag_target_width(0), _edge_grip(nullptr),
      _left_separator(nullptr), _max_frame_time(0), _fps(0),
      _paint_in_this_second(0), _is_idle(true) {
  setObjectName("sliding_drawer");
  setMouseTracking(true);

  // --- Content area (fills the entire drawer widget) ---
  _panel_content = new QWidget(this);
  _panel_content->setObjectName("sliding_drawer_panel_content");

  QVBoxLayout *content_layout = new QVBoxLayout(_panel_content);
  content_layout->setContentsMargins(0, 0, 0, 0);
  content_layout->setSpacing(0);

  // Title bar
  _title_bar = new QWidget(_panel_content);
  _title_bar->setObjectName("sliding_drawer_titlebar");
  _title_bar->setFixedHeight(TITLE_BAR_HEIGHT);

  QHBoxLayout *title_layout = new QHBoxLayout(_title_bar);
  title_layout->setContentsMargins(12, 0, 12, 0);

  _title_label = new QLabel(_title_bar);
  _title_label->setObjectName("sliding_drawer_title");
  title_layout->addWidget(_title_label);
  title_layout->addStretch();

  content_layout->addWidget(_title_bar);

  // Stacked widget for content pages
  _stacked_widget = new QStackedWidget(_panel_content);
  _stacked_widget->setObjectName("sliding_drawer_stack");
  content_layout->addWidget(_stacked_widget, 1);

  // Edge grip for drag-to-resize (left edge of drawer)
  _edge_grip = new QWidget(this);
  _edge_grip->setObjectName("sliding_drawer_edge_grip");
  _edge_grip->setFixedWidth(EDGE_GRIP_WIDTH);
  _edge_grip->setCursor(Qt::SplitHCursor);
  _edge_grip->setAttribute(Qt::WA_TranslucentBackground);
  _edge_grip->installEventFilter(this);
  _edge_grip->raise();

  // Left separator line (independent widget, not covered by scrollbar)
  _left_separator = new QWidget(this);
  _left_separator->setObjectName("DrawerLeftSeparator");
  _left_separator->setFixedWidth(1);
  _left_separator->raise();

  // --- Animation: X-axis translate offset ---
  _animation = new QPropertyAnimation(this, "slideOffset");
  _animation->setDuration(_animation_duration);

  connect(_animation, &QPropertyAnimation::finished, this, [this]() {
    _is_animating = false;
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAttribute(Qt::WA_NoSystemBackground, false);

    if (_is_open) {
      // Opening animation done → apply push margin so tab widget shrinks
      applyPushMargin();
      // Reposition drawer to sit in the margin area (no offset now)
      _slide_offset = 0;
      positionOverlay();
      emit drawerOpened(_current_page);
    } else {
      // Closing animation done → hide and clean up
      finishClose();
      emit drawerClosed();
    }
  });

  // Start hidden: positioned off-screen right
  hide();
  connect(&_fps_timer, &QTimer::timeout, this, [this]() {
    if (_paint_in_this_second > 0) {
      _fps = _max_frame_time;
      _max_frame_time = 0;
      _paint_in_this_second = 0;
    } else {
      _is_idle = true;
    }
  });
  _fps_timer.start(1000);

  _drag_update_timer.setSingleShot(true);
  connect(&_drag_update_timer, &QTimer::timeout, this,
          &SlidingDrawer::applyDragUpdate);

  if (parentWidget())
    parentWidget()->installEventFilter(this);
}

SlidingDrawer::~SlidingDrawer() {}

// ---- Push layout management ----

void SlidingDrawer::setPushLayout(QVBoxLayout *layout) {
  _push_layout = layout;
}

void SlidingDrawer::applyPushMargin() {
  if (!_push_layout)
    return;
  int left = _push_layout->contentsMargins().left();
  _push_layout->setContentsMargins(left, 0, _drawer_width, 0);
}

void SlidingDrawer::removePushMargin() {
  if (!_push_layout)
    return;
  int left = _push_layout->contentsMargins().left();
  _push_layout->setContentsMargins(left, 0, 0, 0);
}

// ---- Page management ----

int SlidingDrawer::addPage(QWidget *content, const QString &title) {
  if (!content)
    return -1;

  content->setParent(_stacked_widget);
  _stacked_widget->addWidget(content);

  PageInfo info;
  info.content = content;
  info.title = title;
  _pages.append(info);

  return _pages.size() - 1;
}

void SlidingDrawer::removePage(int index) {
  if (index < 0 || index >= _pages.size())
    return;

  QWidget *content = _pages[index].content;
  _stacked_widget->removeWidget(content);
  content->setParent(nullptr);
  _pages.removeAt(index);

  if (_current_page == index) {
    close();
    _current_page = -1;
  } else if (_current_page > index) {
    _current_page--;
  }
}

QWidget *SlidingDrawer::page(int index) const {
  if (index < 0 || index >= _pages.size())
    return nullptr;
  return _pages[index].content;
}

int SlidingDrawer::pageCount() const { return _pages.size(); }

// ---- Open / Close ----

void SlidingDrawer::open(int pageIndex) {
  if (pageIndex < 0 || pageIndex >= _pages.size())
    return;

  // If already open with the same page, do nothing
  if (_is_open && _current_page == pageIndex && !_is_animating)
    return;

  // If already open with a DIFFERENT page, just switch content instantly
  if (_is_open && !_is_animating) {
    _current_page = pageIndex;
    _stacked_widget->setCurrentIndex(pageIndex);
    _title_label->setText(_pages[pageIndex].title);
    _pages[pageIndex].content->show();
    emit drawerOpened(pageIndex);
    return;
  }

  // If currently animating, stop it
  if (_is_animating) {
    _animation->stop();
    _is_animating = false;
  }

  _current_page = pageIndex;

  // Update stacked widget and title
  _stacked_widget->setCurrentIndex(pageIndex);
  _title_label->setText(_pages[pageIndex].title);
  _pages[pageIndex].content->show();

  // Ensure push margin is removed (overlay mode during animation)
  removePushMargin();

  // Make drawer visible as overlay, positioned at right edge
  setFixedSize(_drawer_width, parentWidget()->height());
  raise();
  show();

  // Start from current offset (support smooth interrupt)
  int startOffset = _slide_offset;
  if (startOffset <= 0)
    startOffset = _drawer_width; // fully hidden

  _animation->setStartValue(startOffset);
  _animation->setEndValue(0); // slide to fully visible
  _animation->setEasingCurve(QEasingCurve::OutCubic);

  _is_open = true;
  _is_animating = true;
  setAttribute(Qt::WA_OpaquePaintEvent);
  setAttribute(Qt::WA_NoSystemBackground);
  _animation->start();
}

void SlidingDrawer::close() {
  if (!_is_open && !_is_animating)
    return;

  // If currently animating, stop it
  if (_is_animating) {
    _animation->stop();
    _is_animating = false;
  }

  // Step 1: Remove push margin → tab widget expands instantly
  removePushMargin();

  // Drawer is now overlay (tab is full-width, drawer covers right side)

  // Start from current offset (support smooth interrupt)
  int startOffset = _slide_offset;
  if (startOffset >= _drawer_width)
    startOffset = 0; // fully visible

  _animation->setStartValue(startOffset);
  _animation->setEndValue(_drawer_width); // slide to fully hidden
  _animation->setEasingCurve(makeTailwindCurve());

  _is_open = false;
  _is_animating = true;
  setAttribute(Qt::WA_OpaquePaintEvent);
  setAttribute(Qt::WA_NoSystemBackground);
  _animation->start();
}

void SlidingDrawer::toggle(int pageIndex) {
  if (_is_animating) {
    if (_slide_offset > 0) {
      close();
    } else {
      open(pageIndex);
    }
    return;
  }

  if (_is_open && _current_page == pageIndex) {
    close();
  } else {
    open(pageIndex);
  }
}

bool SlidingDrawer::isOpen() const { return _is_open; }

bool SlidingDrawer::isAnimating() const { return _is_animating; }

void SlidingDrawer::setDrawerWidth(int width, bool apply_push) {
  _drawer_width = qMax(MIN_DRAWER_WIDTH, width);
  // If currently open (push mode), update margin and reposition
  if (_is_open && !_is_animating) {
    if (apply_push)
      applyPushMargin();
    setFixedSize(_drawer_width, parentWidget()->height());
    positionOverlay();
  }
}

int SlidingDrawer::drawerWidth() const { return _drawer_width; }

void SlidingDrawer::setAnimationDuration(int ms) {
  _animation_duration = qMax(50, ms);
  _animation->setDuration(_animation_duration);
}

int SlidingDrawer::animationDuration() const { return _animation_duration; }

void SlidingDrawer::setPageTitle(int index, const QString &title) {
  if (index < 0 || index >= _pages.size())
    return;
  _pages[index].title = title;
  if (_current_page == index)
    _title_label->setText(title);
}

// ---- Overlay positioning ----

void SlidingDrawer::positionOverlay() {
  QWidget *pw = parentWidget();
  if (!pw)
    return;

  int pw_width = pw->width();
  int pw_height = pw->height();

  int x = pw_width - _drawer_width + _slide_offset;
  setGeometry(x, 0, _drawer_width, pw_height);
  // Child geometry is handled by resizeEvent — avoid double setGeometry per
  // frame
}

void SlidingDrawer::finishClose() {
  if (_current_page >= 0 && _current_page < _pages.size()) {
    _pages[_current_page].content->hide();
  }

  hide();
  _slide_offset = _drawer_width;
  _current_page = -1;
}

// ---- Property animation: slideOffset ----

int SlidingDrawer::slideOffset() const { return _slide_offset; }

void SlidingDrawer::setSlideOffset(int offset) {
#ifndef NDEBUG
  QElapsedTimer timer;
  timer.start();
#endif

  offset = qBound(0, offset, _drawer_width);
  if (_slide_offset == offset)
    return;

  int oldOffset = _slide_offset;
  _slide_offset = offset;

#ifndef NDEBUG
  QElapsedTimer overlayTimer;
  overlayTimer.start();
#endif
  positionOverlay();
#ifndef NDEBUG
  qint64 t_overlay = overlayTimer.elapsed();
#endif

  QWidget *pw = parentWidget();
#ifndef NDEBUG
  qint64 t_update = 0;
#endif
  if (pw) {
#ifndef NDEBUG
    QElapsedTimer updateTimer;
    updateTimer.start();
#endif
    int pw_width = pw->width();
    int oldX = pw_width - _drawer_width + oldOffset;
    int newX = pw_width - _drawer_width + offset;
    int dirtyLeft = qMin(oldX, newX);
    int dirtyRight = qMax(oldX + _drawer_width, newX + _drawer_width);
    QRect dirtyRect(dirtyLeft, 0, dirtyRight - dirtyLeft, pw->height());
    pw->update(dirtyRect);
#ifndef NDEBUG
    t_update = updateTimer.elapsed();
#endif
  }

#ifndef NDEBUG
  qint64 total = timer.elapsed();
  pxv_warn("[DIAG] SlidingDrawer::setSlideOffset took %lld ms: overlay: %lld "
           "ms, parentUpdate: %lld ms, offset: %d",
           total, t_overlay, t_update, offset);
#endif
}

// ---- Events ----

void SlidingDrawer::paintEvent(QPaintEvent *event) {
#ifndef NDEBUG
  QElapsedTimer timer;
  timer.start();
#endif

  _paint_in_this_second++;
  if (_is_idle || !_frame_interval_timer.isValid()) {
    _frame_interval_timer.restart();
    _is_idle = false;
  } else {
    int elapsed = static_cast<int>(_frame_interval_timer.restart());
    if (elapsed > _max_frame_time) {
      _max_frame_time = elapsed;
    }
  }

  QWidget::paintEvent(event);

#ifndef NDEBUG
  qint64 total = timer.elapsed();
  pxv_warn("[DIAG] SlidingDrawer::paintEvent took %lld ms", total);
#endif
}

void SlidingDrawer::resizeEvent(QResizeEvent *event) {
  Q_UNUSED(event);
  if (_panel_content) {
    _panel_content->setGeometry(0, 0, width(), height());
  }
  if (_edge_grip && height() > 0) {
    _edge_grip->setGeometry(0, 0, EDGE_GRIP_WIDTH, height());
    _edge_grip->raise();
  }
  if (_left_separator && height() > 0) {
    _left_separator->setGeometry(0, 0, 1, height());
    _left_separator->raise();
  }
}

bool SlidingDrawer::eventFilter(QObject *obj, QEvent *event) {
  // Track parent widget resize to reposition the drawer
  if (obj == parentWidget() && event->type() == QEvent::Resize) {
    if (_is_open || _is_animating) {
      setFixedSize(_drawer_width, parentWidget()->height());
      positionOverlay();
    }
  }

  if (obj == _edge_grip && event->type() == QEvent::MouseButtonPress) {
    QMouseEvent *me = static_cast<QMouseEvent *>(event);
    if (me->button() == Qt::LeftButton && _is_open && !_is_animating) {
      _drag_active = true;
      _drag_start_pos = me->globalPosition().toPoint();
      _drag_start_drawer_width = _drawer_width;
      grabMouse(Qt::SplitHCursor);
      return true;
    }
  }

  return QWidget::eventFilter(obj, event);
}

void SlidingDrawer::mouseMoveEvent(QMouseEvent *event) {
  if (_drag_active) {
    if (QWidget::mouseGrabber() != this) {
      finishDrag();
      QWidget::mouseMoveEvent(event);
      return;
    }
    int dx = _drag_start_pos.x() - event->globalPosition().toPoint().x();

    if (qAbs(dx) >= QApplication::startDragDistance()) {
      _drag_target_width =
          qMax(MIN_DRAWER_WIDTH, _drag_start_drawer_width + dx);
      if (!_drag_update_timer.isActive()) {
        applyDragUpdate();
        _drag_update_timer.start(DRAG_FRAME_INTERVAL);
      }
    }
  }

  QWidget::mouseMoveEvent(event);
}

void SlidingDrawer::mousePressEvent(QMouseEvent *event) {
  QWidget::mousePressEvent(event);
}

void SlidingDrawer::mouseReleaseEvent(QMouseEvent *event) {
  if (_drag_active) {
    finishDrag();
    return;
  }

  QWidget::mouseReleaseEvent(event);
}

void SlidingDrawer::finishDrag() {
  _drag_update_timer.stop();
  if (_drag_active && _drag_target_width > 0) {
    setDrawerWidth(_drag_target_width, true);
  }
  bool was_drag = _drag_active;
  _drag_active = false;
  _drag_target_width = 0;
  releaseMouse();
  unsetCursor();

  if (was_drag) {
    Q_EMIT drawerDragFinished();
  }
}

void SlidingDrawer::applyDragUpdate() {
  if (!_drag_active)
    return;
  setDrawerWidth(_drag_target_width, true);
}

int SlidingDrawer::get_fps() const { return _fps; }

} // namespace widgets
} // namespace pv
