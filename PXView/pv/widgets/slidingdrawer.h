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

#ifndef PXVIEW_PV_WIDGETS_SLIDINGDRAWER_H
#define PXVIEW_PV_WIDGETS_SLIDINGDRAWER_H

#include <QWidget>
#include <QPropertyAnimation>
#include <QStackedWidget>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QElapsedTimer>

class QLabel;
class QVBoxLayout;

namespace pv {
namespace widgets {

/**
 * A sliding drawer that uses "overlay during animation, push after animation"
 * — the same pattern as TitleBar's Ribbon panel.
 *
 * Opening:
 *   1. Drawer is an overlay child of its parentWidget(), translates in from right
 *      (no layout recalculation = smooth animation)
 *   2. When animation finishes, set right margin on the push layout
 *      → tab widget shrinks instantly (push mode), drawer now sits in empty space
 *
 * Closing:
 *   1. Remove right margin from push layout → tab widget expands instantly
 *   2. Drawer starts overlay translate-X animation to slide out to the right
 *   3. When animation finishes, drawer is hidden
 *
 * This avoids per-frame layout recalculations (no jank) while still giving
 * the "push" behavior once the animation completes.
 */
class SlidingDrawer : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(int slideOffset READ slideOffset WRITE setSlideOffset)

public:
    explicit SlidingDrawer(QWidget *parent = nullptr);
    ~SlidingDrawer();

    int addPage(QWidget *content, const QString &title = QString());
    void removePage(int index);
    QWidget* page(int index) const;
    int pageCount() const;

    void open(int pageIndex);
    void close();
    void toggle(int pageIndex);

    bool isOpen() const;
    bool isAnimating() const;

    void setDrawerWidth(int width, bool apply_push = true);
    int drawerWidth() const;

    void setAnimationDuration(int ms);
    int animationDuration() const;

    void setPageTitle(int index, const QString &title);

    /**
     * Set the layout whose right margin will be adjusted for push mode.
     * Typically the QVBoxLayout that contains the tab widget.
     */
    void setPushLayout(QVBoxLayout *layout);
    int get_fps() const;

signals:
    void drawerOpened(int pageIndex);
    void drawerClosed();
    void drawerDragFinished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    int slideOffset() const;
    void setSlideOffset(int offset);

    void applyPushMargin();
    void removePushMargin();
    void positionOverlay();
    void finishClose();
    void finishDrag();
    void applyDragUpdate();

    struct PageInfo {
        QWidget *content;
        QString title;
    };

    QVector<PageInfo> _pages;
    QStackedWidget *_stacked_widget;

    // Content area
    QWidget *_panel_content;
    QWidget *_title_bar;
    QLabel *_title_label;

    // Push layout (the QVBoxLayout whose right margin we adjust)
    QVBoxLayout *_push_layout;

    // Animation
    QPropertyAnimation *_animation;
    int _slide_offset;         // 0 = fully visible, _drawer_width = hidden (off-screen right)
    int _drawer_width;
    int _animation_duration;
    int _current_page;
    bool _is_open;
    bool _is_animating;

    // Drag state (edge resize)
    bool _drag_active;
    int _drag_start_drawer_width;
    int _drag_target_width;
    QPoint _drag_start_pos;
    QWidget *_edge_grip;
    QWidget *_left_separator;
    QTimer _drag_update_timer;

    static constexpr int EDGE_GRIP_WIDTH = 6;
    static constexpr int DRAG_FRAME_INTERVAL = 16;

    int _max_frame_time;
    int _fps;
    QTimer _fps_timer;
    QElapsedTimer _frame_interval_timer;
    int _paint_in_this_second;
    bool _is_idle;
};

} // namespace widgets
} // namespace pv

#endif // PXVIEW_PV_WIDGETS_SLIDINGDRAWER_H
