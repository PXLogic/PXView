/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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


#ifndef PXVIEW_PV_VIEW_HEADER_H
#define PXVIEW_PV_VIEW_HEADER_H

#include <list>
#include <utility>
#include <QWidget>
#include <QColor>
#include "../ui/uimanager.h"
#include "../dock/keywordlineedit.h"
#include <QMenu>

namespace pv {
namespace view {

class Trace;
class View;
class LogicSignal;
class DecodeTrace;

//the left panel of main graph
//created by View
class Header : public QWidget, public IUiWindow
{
	Q_OBJECT

public:
	Header(View &parent);

    ~Header();

private:
    pv::view::Trace* get_mTrace(int &action, const QPoint &pt);

private:
    void retranslateUi();
	void paintEvent(QPaintEvent *event);
    void resizeEvent(QResizeEvent *event) override;

private:
	void mousePressEvent(QMouseEvent * event);
	void mouseReleaseEvent(QMouseEvent *event);
	void mouseMoveEvent(QMouseEvent *event);
    void mouseDoubleClickEvent(QMouseEvent *event);
	void leaveEvent(QEvent *event);
    void wheelEvent(QWheelEvent *event);
    void contextMenuEvent(QContextMenuEvent *event);
    void keyPressEvent(QKeyEvent *event) override;

    void changeName(QMouseEvent *event);
    void changeColor(QMouseEvent *event);

    QMenu* create_height_submenu(bool is_batch);

    //IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

public:
    int get_nameEditWidth();
    void header_resize();

    inline bool mouse_is_down(){
        return _mouse_is_down;
    }

private slots:
	void on_action_set_name_triggered();
    void on_reset_row_height();
    void on_reset_all_row_height();
    void on_set_channel_height();
    void on_batch_set_height();
    void on_filter_glitches_triggered();
    void on_clear_channel_filter_triggered();
    void on_clear_all_filter_triggered();
    void on_toggle_invert_triggered();
    void on_change_color_triggered();

signals:
    void traces_moved();
    void header_updated();
    void vDial_changed(quint16);
    void acdc_changed(quint16);
    void ch_changed(quint16);
    void show_glitch_filter_popup(pv::view::LogicSignal* sig);
    void clear_glitch_filter_requested(bool all_channels);
    void toggle_signal_invert_requested(pv::view::LogicSignal* sig);

private:
	View &_view;

    bool _moveFlag;
    bool _colorFlag;
    bool _nameFlag;
	QPoint _mouse_point;
	QPoint _mouse_down_point;
    PopupLineEdit *nameEdit;
    std::list<std::pair<Trace*, int> > _drag_traces;
    Trace *_context_trace;
    Trace       *_resize_trace_upper;
    Trace       *_resize_trace_lower;
    int         _resize_mouse_down_y;
    int         _resize_upper_height;
    int         _resize_lower_height;
    bool    _mouse_is_down;

    // 主动缓存的标题前景色,避免依赖 Qt 的 stylesheet→palette 传播
    // (该传播在启动时序和父级自带 stylesheet 时不可靠,会导致图标变黑)
    QColor  _foreColor;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_HEADER_H
