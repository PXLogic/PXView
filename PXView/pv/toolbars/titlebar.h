/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef PXVIEW_PV_TOOLBARS_TITLEBAR_H
#define PXVIEW_PV_TOOLBARS_TITLEBAR_H

#include <QMenuBar>
#include <QToolButton>
#include <QTabBar>
#include <QPropertyAnimation>
#include <QAction>
#include <QWidgetAction>
#include <QStackedWidget>

#include "../interface/icallbacks.h"
#include "../ui/uimanager.h"


class QHBoxLayout;
class QLabel;
class QVBoxLayout;

namespace pv {

class ITitleParent
{
public:
    virtual void MoveWindow(int x, int y)=0;
    virtual QPoint GetParentPos()=0;
    virtual bool ParentIsMaxsized()=0;
    virtual void MoveBegin()=0;
    virtual void MoveEnd()=0;
    virtual void ParentShowMaximized()=0;
    virtual void ParentShowNormal()=0;
};

namespace toolbars {

class TitleBar : public QMenuBar, public IUiWindow
{
    Q_OBJECT
    Q_PROPERTY(int slideOffset READ slideOffset WRITE setSlideOffset)

public:
    TitleBar(bool top, QWidget *parent, ITitleParent *titleParent, bool hasClose, bool enableRibbon = true);
    ~TitleBar();

    void setTitle(QString title);
    QString title();

    int addCategory(const QString &title);
    void addAction(int categoryIndex, QAction *action);
    void addSeparator(int categoryIndex);
    void retranslateUi(int categoryIndex, const QString &title);
    void expandRibbon();
    void hideRibbon();
    void expandRibbonPinned();
    void hideRibbonPinned();

    //IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

    inline void set_native(){
        _is_native = true;
    }

    inline void update_font(){
        UpdateFont();
    }

    void EnableAbleDrag(bool bEnabled);

    int slideOffset() const;
    void setSlideOffset(int offset);

    inline bool isRibbonAnimating() const {
        return _ribbonAnimation && _ribbonAnimation->state() == QAbstractAnimation::Running;
    }

private:
    void reStyle();
    void positionPinButton();
    void updateBottomLine();

    bool ParentIsMaxsized();
    bool isOnTabBar(const QPoint &pos) const;
    void positionRibbonContainer();

public slots:
    void showMaxRestore();
    void setRestoreButton(bool max);
    inline bool IsMoving(){return _moving;}

private slots:
    void onTabClicked(int index);
    void onTabChanged(int index);
    void onPinToggled(bool checked);

protected:
    void paintEvent(QPaintEvent *event);
    void resizeEvent(QResizeEvent *event);
    void mousePressEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);
    void mouseDoubleClickEvent(QMouseEvent *event);
    bool eventFilter(QObject *watched, QEvent *event) override;


    QToolButton *_minimizeButton;
    QToolButton *_maximizeButton;
    QToolButton *_closeButton;
    QLabel      *_title;
    QTabBar     *_tabBar;

    QWidget         *_ribbonContainer;
    QWidget         *_ribbonPanel;
    QVBoxLayout     *_ribbonLayout;
    QStackedWidget  *_categoryStack;
    QList<QHBoxLayout*> _categoryLayouts;
    QPropertyAnimation *_ribbonAnimation;
    bool            _ribbonExpanded;
    int             _ribbonExpandedHeight;
    int             _slideOffset;

    QToolButton     *_pinButton;
    bool            _ribbonPinned;
    QWidget         *_bottomLine;

    bool        _moving;
    bool        _is_draging;
    bool        _isTop;
    bool        _hasClose;
    QPoint      _clickPos;
    QPoint      _oldPos;
    QWidget     *_parent;
    bool        _is_native;
    ITitleParent    *_titleParent;
    bool        _is_done_moved;
    bool        _is_able_drag;
    bool        _enableRibbon;
};

} // namespace toolbars
} // namespace pv

#endif // PXVIEW_PV_TOOLBARS_TITLEBAR_H
