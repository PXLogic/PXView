/*
 * This file is part of the PXView project.
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PXVIEW_PV_WIDGETS_SIDEBARBUTTON_H
#define PXVIEW_PV_WIDGETS_SIDEBARBUTTON_H

#include <QString>
#include <QWidget>
#include <QEnterEvent>

class SideBarButton : public QWidget {
  Q_OBJECT

public:
  enum ItemType { DockItem, ActionItem };

  explicit SideBarButton(QWidget *parent = nullptr);

  void setIconName(const QString &name);
  QString iconName() const { return _iconName; }

  void setAlternateIconName(const QString &name);
  QString alternateIconName() const { return _alternateIconName; }

  void setText(const QString &text);
  QString text() const { return _text; }

  void setCheckable(bool checkable);
  bool isCheckable() const { return _isCheckable; }

  void setChecked(bool checked);
  bool isChecked() const { return _isChecked; }

  void setRunning(bool running);
  bool isRunning() const { return _isRunning; }

  void setItemType(ItemType type);
  ItemType itemType() const { return _itemType; }

  void setDrawerPageIndex(int page);
  int drawerPageIndex() const { return _drawerPageIndex; }

  void setEnabled(bool enabled);
  bool isEnabled() const { return _isEnabled; }

  void setVisible(bool visible);

  QSize sizeHint() const override;

  QRectF indicatorRect() const;

  void click();

signals:
  void clicked();

protected:
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;

private:
  void drawBackground(QPainter *painter);
  void drawIcon(QPainter *painter);
  void drawText(QPainter *painter);

  static QString stripShortcut(const QString &text);

  QString _iconName;
  QString _alternateIconName;
  QString _text;
  bool _isCheckable;
  bool _isChecked;
  bool _isRunning;
  bool _isHovered;
  bool _isPressed;
  bool _isEnabled;
  ItemType _itemType;
  int _drawerPageIndex;
};

#endif
