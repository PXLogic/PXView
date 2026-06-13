/*
 * This file is part of the PXView project.
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "sidebar.h"
#include "../config/appconfig.h"
#include "../ui/langresource.h"
#include "../ui/dockfonts.h"
#include <QFrame>
#include <QIcon>
#include <QPainter>
#include <QResizeEvent>

namespace pv {
namespace widgets {

ScaleSlideAnimation::ScaleSlideAnimation(QObject *parent,
                                         Qt::Orientation orient)
    : QObject(parent), _orient(orient), _currentAni(nullptr) {
  if (isHorizontal()) {
    _geometry = QRectF(0, 0, 16, 3);
  } else {
    _geometry = QRectF(0, 0, 3, 16);
  }

  _slideAniGroup = new QParallelAnimationGroup(this);
  _crossAniGroup = new QParallelAnimationGroup(this);

  _slidePosAni1 = new QPropertyAnimation(this, "pos", this);
  _slidePosAni2 = new QPropertyAnimation(this, "pos", this);
  _slideLengthAni1 = new QPropertyAnimation(this, "length", this);
  _slideLengthAni2 = new QPropertyAnimation(this, "length", this);
  _seqSlidePosAniGroup = new QSequentialAnimationGroup(this);
  _seqLengthAniGroup = new QSequentialAnimationGroup(this);

  _crossLenAni = new QPropertyAnimation(this, "length", this);
  _crossPosAni = new QPropertyAnimation(this, "pos", this);

  _seqSlidePosAniGroup->addAnimation(_slidePosAni1);
  _seqSlidePosAniGroup->addAnimation(_slidePosAni2);
  _seqLengthAniGroup->addAnimation(_slideLengthAni1);
  _seqLengthAniGroup->addAnimation(_slideLengthAni2);

  _slideAniGroup->addAnimation(_seqSlidePosAniGroup);
  _slideAniGroup->addAnimation(_seqLengthAniGroup);

  _crossAniGroup->addAnimation(_crossLenAni);
  _crossAniGroup->addAnimation(_crossPosAni);

  connect(_slideAniGroup, &QParallelAnimationGroup::finished, this,
          &ScaleSlideAnimation::finished);
  connect(_crossAniGroup, &QParallelAnimationGroup::finished, this,
          &ScaleSlideAnimation::finished);
}

void ScaleSlideAnimation::setPos(const QPointF &pos) {
  _geometry.moveTopLeft(pos);
  emit valueChanged(_geometry);
}

float ScaleSlideAnimation::getLength() const {
  return isHorizontal() ? _geometry.width() : _geometry.height();
}

void ScaleSlideAnimation::setLength(float length) {
  if (isHorizontal()) {
    _geometry.setWidth(length);
  } else {
    _geometry.setHeight(length);
  }
  emit valueChanged(_geometry);
}

void ScaleSlideAnimation::setGeometry(const QRectF &rect) {
  _geometry = rect;
  emit valueChanged(_geometry);
}

QEasingCurve ScaleSlideAnimation::createBezierCurve(float x1, float y1,
                                                     float x2, float y2) {
  QEasingCurve curve(QEasingCurve::BezierSpline);
  curve.addCubicBezierSegment(QPointF(x1, y1), QPointF(x2, y2),
                              QPointF(1, 1));
  return curve;
}

void ScaleSlideAnimation::startAnimation(const QRectF &endRect,
                                         bool useCrossFade) {
  stopAnimation();

  // Reset geometry to default before starting new animation
  // to prevent accumulated state from causing indicator stretching
  if (isHorizontal()) {
    _geometry = QRectF(_geometry.x(), _geometry.y(), 16, 3);
  } else {
    _geometry = QRectF(_geometry.x(), _geometry.y(), 3, 16);
  }

  QRectF startRect = _geometry;

  bool sameLevel;
  qreal dim, from, to;

  if (isHorizontal()) {
    sameLevel = qAbs(startRect.y() - endRect.y()) < 1;
    dim = startRect.width();
    from = startRect.x();
    to = endRect.x();
  } else {
    sameLevel = qAbs(startRect.x() - endRect.x()) < 1;
    dim = startRect.height();
    from = startRect.y();
    to = endRect.y();
  }

  if (sameLevel && !useCrossFade) {
    startSlideAnimation(startRect, endRect, from, to, dim);
  } else {
    startCrossFadeAnimation(startRect, endRect);
  }
}

void ScaleSlideAnimation::stopAnimation() {
  _slideAniGroup->stop();
  _crossAniGroup->stop();
}

void ScaleSlideAnimation::startSlideAnimation(const QRectF &startRect,
                                              const QRectF &endRect,
                                              qreal from, qreal to,
                                              qreal dimension) {
  _currentAni = _slideAniGroup;

  _slidePosAni1->setDuration(200);
  _slidePosAni2->setDuration(400);
  _slidePosAni1->setEasingCurve(
      createBezierCurve(0.9f, 0.1f, 1.0f, 0.2f));
  _slidePosAni2->setEasingCurve(
      createBezierCurve(0.1f, 0.9f, 0.2f, 1.0f));

  _slideLengthAni1->setDuration(200);
  _slideLengthAni2->setDuration(400);
  _slideLengthAni1->setEasingCurve(
      createBezierCurve(0.9f, 0.1f, 1.0f, 0.2f));
  _slideLengthAni2->setEasingCurve(
      createBezierCurve(0.1f, 0.9f, 0.2f, 1.0f));

  qreal dist = qAbs(to - from);
  qreal midLength = dist + dimension;
  bool isForward = to > from;

  QPointF startPos = startRect.topLeft();
  QPointF endPos = endRect.topLeft();

  if (isForward) {
    _slidePosAni1->setStartValue(startPos);
    _slidePosAni1->setEndValue(startPos);
    _slideLengthAni1->setStartValue(dimension);
    _slideLengthAni1->setEndValue(midLength);

    _slidePosAni2->setStartValue(startPos);
    _slidePosAni2->setEndValue(endPos);
    _slideLengthAni2->setStartValue(midLength);
    _slideLengthAni2->setEndValue(dimension);
  } else {
    _slidePosAni1->setStartValue(startPos);
    _slidePosAni1->setEndValue(endPos);
    _slideLengthAni1->setStartValue(dimension);
    _slideLengthAni1->setEndValue(midLength);

    _slidePosAni2->setStartValue(endPos);
    _slidePosAni2->setEndValue(endPos);
    _slideLengthAni2->setStartValue(midLength);
    _slideLengthAni2->setEndValue(dimension);
  }

  _slideAniGroup->start();
}

void ScaleSlideAnimation::startCrossFadeAnimation(const QRectF &startRect,
                                                  const QRectF &endRect) {
  _currentAni = _crossAniGroup;
  setGeometry(endRect);

  bool isNextBelow =
      isHorizontal() ? (endRect.x() > startRect.x()) : (endRect.y() > startRect.y());

  QRectF startGeo;
  qreal dim;

  if (isHorizontal()) {
    dim = endRect.width();
    startGeo = QRectF(endRect.x() + (isNextBelow ? 0 : dim), endRect.y(), 0,
                      endRect.height());
  } else {
    dim = endRect.height();
    startGeo = QRectF(endRect.x(),
                      endRect.y() + (isNextBelow ? 0 : dim), endRect.width(),
                      0);
  }

  setGeometry(startGeo);

  _crossLenAni->setDuration(600);
  _crossLenAni->setStartValue(0.0);
  _crossLenAni->setEndValue(dim);
  _crossLenAni->setEasingCurve(QEasingCurve::OutQuint);

  _crossPosAni->setDuration(600);
  _crossPosAni->setStartValue(startGeo.topLeft());
  _crossPosAni->setEndValue(endRect.topLeft());
  _crossPosAni->setEasingCurve(QEasingCurve::OutQuint);

  _crossAniGroup->start();
}

SideBarIndicator::SideBarIndicator(QWidget *parent) : QWidget(parent) {
  resize(3, 16);
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setAttribute(Qt::WA_TranslucentBackground);
  hide();

  _scaleAni = new ScaleSlideAnimation(this, Qt::Vertical);

  connect(_scaleAni, &ScaleSlideAnimation::valueChanged, this,
          [this](const QRectF &g) {
            setGeometry(g.toRect());
            show();
            raise();
          });

  connect(_scaleAni, &ScaleSlideAnimation::finished, this,
          &SideBarIndicator::aniFinished);
}

void SideBarIndicator::startAnimation(const QRectF &startRect,
                                      const QRectF &endRect,
                                      bool useCrossFade) {
  _scaleAni->stopAnimation();
  setGeometry(startRect.toRect());
  show();
  raise();

  _scaleAni->setGeometry(startRect);
  _scaleAni->startAnimation(endRect, useCrossFade);
}

void SideBarIndicator::stopAnimation() {
  _scaleAni->stopAnimation();
  hide();
}

void SideBarIndicator::snapTo(const QRectF &rect) {
  _scaleAni->stopAnimation();
  _scaleAni->setGeometry(rect);
  setGeometry(rect.toRect());
  show();
  raise();
}

void SideBarIndicator::hideIndicator() {
  _scaleAni->stopAnimation();
  hide();
}

void SideBarIndicator::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event);

  QPainter painter(this);
  painter.setRenderHints(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);

  // Draw main indicator bar with soft color
  QColor accentColor = AppConfig::Instance().GetThemeColor("@sidebar-accent");
  if (!accentColor.isValid()) accentColor = QColor("#5B8DEF"); // fallback
  painter.setBrush(accentColor);
  painter.drawRoundedRect(rect(), 1.5, 1.5);
}

SideBar::SideBar(QWidget *parent)
    : QToolBar(parent), _next_index(0), _checked_index(-1) {
  setMovable(false);
  setObjectName("sidebar");
  setFixedWidth(60);

  _container = new QWidget(this);
  _container->setFixedWidth(60);
  _layout = new QVBoxLayout(_container);
  _layout->setContentsMargins(0, 10, 0, 10);
  _layout->setSpacing(10);
  _layout->addStretch();

  addWidget(_container);

  _indicator = new SideBarIndicator(_container);
  connect(_indicator, &SideBarIndicator::aniFinished, this,
          &SideBar::onIndicatorAniFinished);

  ADD_UI(this);
}

SideBar::~SideBar() { REMOVE_UI(this); }

int SideBar::addItem(const QString &iconName, const char *textId,
                     const QString &defaultText, ItemType type,
                     int drawerPageIndex, const QString &alternateIconName) {
  SideBarButton *btn = new SideBarButton(_container);
  btn->setIconName(iconName);
  btn->setText(L_S(STR_PAGE_TOOLBAR, textId, defaultText.toUtf8().constData()));
  btn->setItemType(static_cast<SideBarButton::ItemType>(type));
  btn->setDrawerPageIndex(drawerPageIndex);

  if (type == DockItem)
    btn->setCheckable(true);

  if (!alternateIconName.isEmpty())
    btn->setAlternateIconName(alternateIconName);

  _layout->insertWidget(_layout->count() - 1, btn, 0, Qt::AlignHCenter);

  connect(btn, &SideBarButton::clicked, this, &SideBar::onButtonClicked);

  ItemInfo info;
  info.index = _next_index;
  info.type = type;
  info.iconName = iconName;
  info.alternateIconName = alternateIconName;
  info.textId = textId;
  info.defaultText = defaultText;
  info.button = btn;
  info.drawerPageIndex = drawerPageIndex;
  _items.append(info);

  return _next_index++;
}

void SideBar::addSeparator() {
  QFrame *line = new QFrame(_container);
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);
  line->setFixedHeight(1);
  _layout->insertWidget(_layout->count() - 1, line);
}

QRectF SideBar::getIndicatorRect(SideBarButton *btn) const {
  if (!btn)
    return QRectF();
  QRectF btnRect = btn->indicatorRect();
  QPoint pos = btn->mapTo(_container, QPoint(0, 0));
  return btnRect.translated(pos);
}

void SideBar::onButtonClicked() {
  SideBarButton *btn = qobject_cast<SideBarButton *>(sender());
  if (!btn)
    return;

  for (int i = 0; i < _items.size(); i++) {
    if (_items[i].button == btn) {
      if (_items[i].type == DockItem) {
        // If clicking the already active dock item, uncheck it and close drawer
        if (_items[i].index == _checked_index) {
          btn->setChecked(false);
          _indicator->hideIndicator();
          _checked_index = -1;
          emit dockItemClicked(_items[i].index);
          return;
        }

        SideBarButton *prevBtn = nullptr;
        for (int j = 0; j < _items.size(); j++) {
          if (_items[j].index == _checked_index && _items[j].type == DockItem) {
            prevBtn = _items[j].button;
          }
          if (j != i && _items[j].type == DockItem)
            _items[j].button->setChecked(false);
        }
        btn->setChecked(true);

        QRectF endRect = getIndicatorRect(btn);

        if (prevBtn && prevBtn != btn) {
          QRectF startRect = _indicator->currentGeometry();
          if (!startRect.isValid())
            startRect = getIndicatorRect(prevBtn);
          _indicator->startAnimation(startRect, endRect);
        } else {
          _indicator->snapTo(endRect);
        }

        _checked_index = _items[i].index;
        emit dockItemClicked(_items[i].index);
      } else {
        emit actionItemClicked(_items[i].index);
      }
      break;
    }
  }
}

void SideBar::onIndicatorAniFinished() {}

void SideBar::setItemVisible(int index, bool visible) {
  for (auto &item : _items) {
    if (item.index == index) {
      item.button->setVisible(visible);
      break;
    }
  }
}

void SideBar::setItemEnabled(int index, bool enabled) {
  for (auto &item : _items) {
    if (item.index == index) {
      item.button->setEnabled(enabled);
      break;
    }
  }
}

void SideBar::setItemChecked(int index, bool checked) {
  for (auto &item : _items) {
    if (item.index == index) {
      item.button->setChecked(checked);
      if (checked && item.type == DockItem) {
        _checked_index = index;
        QTimer::singleShot(0, this, [this]() {
          if (_checked_index >= 0) {
            for (auto &it : _items) {
              if (it.index == _checked_index && it.type == DockItem) {
                _indicator->snapTo(getIndicatorRect(it.button));
                break;
              }
            }
          }
        });
      }
      break;
    }
  }
}

void SideBar::setItemRunning(int index, bool running) {
  for (auto &item : _items) {
    if (item.index == index) {
      item.button->setRunning(running);
      if (item.defaultText == "Start") {
        item.button->setText(running ? L_S(STR_PAGE_TOOLBAR, "IDS_TOOLBAR_RUN_STOP", "Stop") 
                                     : L_S(STR_PAGE_TOOLBAR, item.textId, "Start"));
      } else if (item.defaultText == "Instant") {
        item.button->setText(running ? L_S(STR_PAGE_TOOLBAR, "IDS_TOOLBAR_ONE_STOP", "Stop") 
                                     : L_S(STR_PAGE_TOOLBAR, item.textId, "Instant"));
      }
      break;
    }
  }
}

void SideBar::clearAllChecked() {
  for (auto &item : _items) {
    if (item.type == DockItem)
      item.button->setChecked(false);
  }
  _indicator->hideIndicator();
  _checked_index = -1;
}

int SideBar::itemCount() const { return _items.size(); }

const SideBar::ItemInfo *SideBar::getItem(int index) const {
  for (int i = 0; i < _items.size(); i++) {
    if (_items[i].index == index)
      return &_items[i];
  }
  return nullptr;
}

void SideBar::UpdateLanguage() {
  for (auto &item : _items) {
    item.button->setText(L_S(STR_PAGE_TOOLBAR, item.textId,
                             item.defaultText.toUtf8().constData()));
  }
}

void SideBar::UpdateTheme() {
  for (auto &item : _items) {
    item.button->setIconName(item.iconName);
    if (!item.alternateIconName.isEmpty())
      item.button->setAlternateIconName(item.alternateIconName);
  }
}

void SideBar::UpdateFont() {
  QFont font = theme_font_sidebar();
  for (auto &item : _items) {
    item.button->setFont(font);
  }
}

void SideBar::resizeEvent(QResizeEvent *event) {
  QToolBar::resizeEvent(event);
  if (_checked_index >= 0) {
    for (const auto &item : _items) {
      if (item.index == _checked_index && item.type == DockItem) {
        _indicator->snapTo(getIndicatorRect(item.button));
        QTimer::singleShot(0, this, [this]() {
          if (_checked_index >= 0) {
            for (const auto &it : _items) {
              if (it.index == _checked_index && it.type == DockItem) {
                _indicator->snapTo(getIndicatorRect(it.button));
                break;
              }
            }
          }
        });
        break;
      }
    }
  }
}

} // namespace widgets
} // namespace pv
