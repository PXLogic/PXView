/*
 * This file is part of the PXView project.
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PXVIEW_PV_WIDGETS_SIDEBAR_H
#define PXVIEW_PV_WIDGETS_SIDEBAR_H

#include "../ui/uimanager.h"
#include "sidebarbutton.h"
#include <QList>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QToolBar>
#include <QTimer>
#include <QVBoxLayout>

class QFrame;

namespace pv {
namespace widgets {

class ScaleSlideAnimation : public QObject {
  Q_OBJECT
  Q_PROPERTY(QPointF pos READ getPos WRITE setPos)
  Q_PROPERTY(float length READ getLength WRITE setLength)
  Q_PROPERTY(QRectF geometry READ getGeometry WRITE setGeometry)

public:
  explicit ScaleSlideAnimation(QObject *parent = nullptr,
                               Qt::Orientation orient = Qt::Vertical);

  void startAnimation(const QRectF &endRect, bool useCrossFade = false);
  void stopAnimation();

  bool isHorizontal() const { return _orient == Qt::Horizontal; }

  QPointF getPos() const { return _geometry.topLeft(); }
  void setPos(const QPointF &pos);

  float getLength() const;
  void setLength(float length);

  QRectF getGeometry() const { return _geometry; }
  void setGeometry(const QRectF &rect);

signals:
  void valueChanged(const QRectF &rect);
  void finished();

private:
  void startSlideAnimation(const QRectF &startRect, const QRectF &endRect,
                           qreal from, qreal to, qreal dimension);
  void startCrossFadeAnimation(const QRectF &startRect, const QRectF &endRect);

  static QEasingCurve createBezierCurve(float x1, float y1, float x2,
                                        float y2);

  Qt::Orientation _orient;
  QRectF _geometry;

  QParallelAnimationGroup *_slideAniGroup;
  QParallelAnimationGroup *_crossAniGroup;
  QObject *_currentAni;

  QPropertyAnimation *_slidePosAni1;
  QPropertyAnimation *_slidePosAni2;
  QPropertyAnimation *_slideLengthAni1;
  QPropertyAnimation *_slideLengthAni2;
  QSequentialAnimationGroup *_seqSlidePosAniGroup;
  QSequentialAnimationGroup *_seqLengthAniGroup;

  QPropertyAnimation *_crossLenAni;
  QPropertyAnimation *_crossPosAni;
};

class SideBarIndicator : public QWidget {
  Q_OBJECT

public:
  explicit SideBarIndicator(QWidget *parent = nullptr);

  void startAnimation(const QRectF &startRect, const QRectF &endRect,
                      bool useCrossFade = false);
  void stopAnimation();
  void snapTo(const QRectF &rect);
  void hideIndicator();

  QRectF currentGeometry() const { return _scaleAni->getGeometry(); }

signals:
  void aniFinished();

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  ScaleSlideAnimation *_scaleAni;
};

class SideBar : public QToolBar, public IUiWindow {
  Q_OBJECT

public:
  enum ItemType { DockItem, ActionItem };

  struct ItemInfo {
    int index;
    ItemType type;
    QString iconName;
    QString alternateIconName;
    const char *textId;
    QString defaultText;
    SideBarButton *button;
    int drawerPageIndex;
  };

  explicit SideBar(QWidget *parent = nullptr);
  ~SideBar();

  int addItem(const QString &iconName, const char *textId,
              const QString &defaultText, ItemType type = DockItem,
              int drawerPageIndex = -1,
              const QString &alternateIconName = QString());
  void addSeparator();

  void setItemVisible(int index, bool visible);
  void setItemEnabled(int index, bool enabled);
  void setItemChecked(int index, bool checked);
  void setItemRunning(int index, bool running);
  void clearAllChecked();

  int itemCount() const;
  const ItemInfo *getItem(int index) const;

signals:
  void dockItemClicked(int index);
  void actionItemClicked(int index);

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  void onButtonClicked();
  void onIndicatorAniFinished();

  QRectF getIndicatorRect(SideBarButton *btn) const;

  void UpdateLanguage() override;
  void UpdateTheme() override;
  void UpdateFont() override;

  QVBoxLayout *_layout;
  QWidget *_container;
  QList<ItemInfo> _items;
  int _next_index;
  SideBarIndicator *_indicator;
  int _checked_index;
};

} // namespace widgets
} // namespace pv

#endif
