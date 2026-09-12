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

#include "pv/view/component/ruler.h"
#include "pv/view/component/ruler_format.h"
#include "pv/view/component/ruler_paint.h"
#include <cassert>
#include <cmath>
#include <limits.h>
#include <cmath>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOption>
#include "pv/view/cursor/cursor.h"
#include "pv/view/view.h"
#include "pv/view/viewport/viewport.h"
#include "pv/session/sigsession.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/base/pxvdef.h"
#include "pv/config/appconfig.h"
#include "pv/ui/fn.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/uimanager.h"


using namespace std;
using namespace Qt;

namespace pv {
namespace view {

const int Ruler::MinorTickSubdivision = 4;
const int Ruler::ScaleUnits[3] = {1, 2, 5};
const int Ruler::MinPeriodScale = 10;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const QString Ruler::SIPrefixes[9] =
	{"f", "p", "n", QChar(0x03BC), "m", "", "k", "M", "G"};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const QString Ruler::FreqPrefixes[9] =
    {"P", "T", "G", "M", "K", "", "", "", ""};
const int Ruler::FirstSIPrefixPower = -15;
const int Ruler::pricision = 2;

const int Ruler::HoverArrowSize = 4;

const int Ruler::CursorSelWidth = 20;

const int Ruler::CursorHsbColorTable[CURSOR_HSB_COLOR_TABLE_LENGTH] = {
    120,
    195,
    270,
    345,
    60, //5
    135,
    210,
    285,
    15,
    75, //10
    150,
    225,
    300,
    30,
    90, //15
    165,
    240,
    315,
    45,
    105, //20
    180,
    255,
};

Ruler::Ruler(View &parent) :
	QWidget(&parent),
	_view(parent),
    _cursor_sel_visible(false),
    _cursor_go_visible(false),
    _cursor_sel_x(-1),
    _grabbed_marker(nullptr),
    _hitCursor(false),
    _curs_moved(false)
{
	setMouseTracking(true);
    _foreColor = QColor();  // 无效色,UpdateTheme 会填充

	connect(&_view, &View::hover_point_changed,
		this, &Ruler::hover_point_changed);

    ADD_UI(this);
}

Ruler::~Ruler()
{
    REMOVE_UI(this);
}

// IUiWindow:Ruler 之前未注册到 UiManager,主题切换时不会收到通知,
// 只能被动依赖 palette 传播(同样不可靠)。现统一接入主题更新链路。
void Ruler::UpdateLanguage() { update(); }
void Ruler::UpdateFont() {}
void Ruler::UpdateTheme()
{
    // 主动从主题 token 读取前景色,不再被动依赖 QWidget::palette()。
    // 与 Header 同因:QSS 的 color→palette 传播在启动时序、父级自带
    // stylesheet、事件处理顺序三场景下不可靠,会导致刻度文字/悬停箭头
    // 在暗色主题下变黑不可见,且改主题走同一路径仍会失败。
    _foreColor = AppConfig::Instance().GetThemeColor("@fg-base");
    update();
}

QColor Ruler::GetColorByCursorOrder(int order)
{
    // Task 3.2: body moved to widget-free pv::view::cursor_hsb_color
    // (ruler_format.cpp) so pxview-render cursor paint code can link it.
    return pv::view::cursor_hsb_color(order);
}

QString Ruler::format_freq(double period, unsigned int precision)
{
    return pv::view::format_freq(period, precision);
}

QString Ruler::format_time(double t, int prefix,
    unsigned int precision)
{
    return pv::view::format_time(t, prefix, precision);
}

QString Ruler::format_time(double t)
{
    return format_time(t, _cur_prefix);
}

QString Ruler::format_real_time(uint64_t delta_index, uint64_t sample_rate)
{
    return pv::view::format_real_time(delta_index, sample_rate);
}

QString Ruler::format_real_freq(uint64_t delta_index, uint64_t sample_rate)
{
    return pv::view::format_real_freq(delta_index, sample_rate);
}

TimeMarker* Ruler::get_grabbed_cursor()
{
    return _grabbed_marker;
}

void Ruler::set_grabbed_cursor(TimeMarker *grabbed_marker)
{
    _grabbed_marker = grabbed_marker;
    _grabbed_marker->set_grabbed(true);
}

void Ruler::rel_grabbed_cursor()
{
    if (_grabbed_marker) {
        _grabbed_marker->set_grabbed(false);
        _grabbed_marker = nullptr;
    }
}

void Ruler::paintEvent(QPaintEvent*)
{   
    if (_view.get_view_width() <= 0) {
        return;
    }

    QStyleOption o;
    o.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &o, &p, this);

    QFont font = theme_font_ruler();
    p.setFont(font);
    p.setRenderHint(QPainter::TextAntialiasing, false);

    SigSession *session = &_view.session();

    // Draw tick mark
    if (session->device()->get_work_mode() == DSO)
        draw_osc_tick_mark(p);
    else
        draw_logic_tick_mark(p);

    p.setRenderHint(QPainter::Antialiasing, true);
	// Draw the hover mark
	draw_hover_mark(p);

    // Draw cursor selection
    if (_cursor_sel_visible || _cursor_go_visible) {
        draw_cursor_sel(p);
    }

	p.end();
}

void Ruler::mouseMoveEvent(QMouseEvent *e)
{
    (void)e;

    if (_grabbed_marker) {
        int msx = _view.hover_point().x();
        if (msx < 0)
            msx = 0;   
        int body_width = _view.get_body_width();
        if (msx > body_width)
            msx = body_width;

        uint64_t index = _view.pixel2index(msx);
        _grabbed_marker->set_index(index);
        _view.cursor_moving();
        _curs_moved = true;
    }

    update();
    _view.viewport()->update();
}

void Ruler::leaveEvent(QEvent *)
{
    _cursor_sel_visible = false;
    _cursor_go_visible = false;
    update();
}

void Ruler::mousePressEvent(QMouseEvent *event)
{
    if (event->button() & Qt::LeftButton) {
        bool visible;
        auto &cursor_list = _view.get_cursorList();

        if (!_cursor_sel_visible && cursor_list.size()) {
            _view.show_cursors(true);
            auto i = cursor_list.begin();

            while (i != cursor_list.end()) {
                const QRect cursor_rect((*i)->get_label_rect(rect(), visible));
                if ((*i)->get_close_rect(cursor_rect).contains(event->position().toPoint())) {
                    _view.del_cursor(i->get());

                    if (cursor_list.empty()) {
                        _cursor_sel_visible = false;
                        _view.show_cursors(false);
                    }
                    _hitCursor = true;
                    break;
                }

                if (cursor_rect.contains(event->position().toPoint())) {
                    set_grabbed_cursor(i->get());
                    _cursor_sel_visible = false;
                    _cursor_go_visible = false;
                    _hitCursor = true;
                    break;
                }
                i++;
            }
        }
    }
}

void Ruler::mouseReleaseEvent(QMouseEvent *event)
{
    bool updatedCursor = false;

    if (event->button() & Qt::LeftButton) {
        if (!_hitCursor && !_grabbed_marker) {
            if (!_cursor_go_visible) {
                if (!_cursor_sel_visible) {
                    _cursor_sel_x = event->position().toPoint().x();
                    _cursor_sel_visible = true;                                    
                } 
                else {
                    int overCursor;
                    int msx = _cursor_sel_x;
                    if (msx < 0)
                        msx = 0;

                    int body_width = _view.get_body_width();
                    if (msx > body_width)
                        msx = body_width;

                    auto &cursor_list = _view.get_cursorList();
                    uint64_t index = _view.pixel2index(msx);
                    overCursor = in_cursor_sel_rect(event->position().toPoint());

                    if (overCursor == 0) {
                        _view.add_cursor(index);
                        _view.show_cursors(true);
                        updatedCursor = true;
                    }
                    else if (overCursor > 0) {
                        auto i = cursor_list.begin();

                        while (--overCursor != 0){
                            i++;
                        }

                        (*i)->set_index(index);
                        updatedCursor = true;
                        _view.cursor_moved();
                    }
                    _cursor_sel_visible = false;
                }
            } 
            else {
                int overCursor;
                overCursor = in_cursor_sel_rect(event->position().toPoint());
                if (overCursor > 0) {
                    _view.set_cursor_middle(overCursor - 1);
                }

                _cursor_go_visible = false;
            }
        }

        if (_curs_moved && _grabbed_marker) {
            rel_grabbed_cursor();
            _hitCursor = false;
            _curs_moved = false;
            _view.cursor_moved();
        }

        if (_hitCursor && !_grabbed_marker) {
            _hitCursor = false;
        }
    }

    if (event->button() & Qt::RightButton) {
        if (!_cursor_sel_visible) {
            if (!_cursor_go_visible) {
                _cursor_sel_x = event->position().toPoint().x();
                _cursor_go_visible = true;
            }
        } else {
            int overCursor;
            overCursor = in_cursor_sel_rect(event->position().toPoint());
            auto &cursor_list = _view.get_cursorList();

            if (overCursor > 0) {
                auto i = cursor_list.begin();

                while (--overCursor != 0){
                    i++;
                }

                _view.del_cursor(i->get());
            }

            if (cursor_list.empty()) {
                _cursor_sel_visible = false;
                _view.show_cursors(false);
            }
        }
    }

    update();

    if (updatedCursor) {
        _view.viewport()->update();
    }
}

void Ruler::draw_logic_tick_mark(QPainter &p)
{
    // Task 3.3: body moved verbatim to widget-free pv::view::paint_logic_tick_mark
    // (ruler_paint.cpp, pxview-render) so the QML shell's RulerItem shares the
    // same tick-mark logic. Only dependency injection changed:
    // _foreColor (palette fallback) -> resolved fore parameter,
    // height()/rect() -> rect parameter, _min_period/_cur_prefix -> out-params.
    QColor fore = _foreColor.isValid()
                      ? _foreColor
                      : QWidget::palette().color(QWidget::foregroundRole());
    pv::view::paint_logic_tick_mark(p, _view, rect(), fore,
        _min_period, _cur_prefix);
}

void Ruler::draw_osc_tick_mark(QPainter &p)
{
    // Task 3.3: body moved verbatim to widget-free pv::view::paint_osc_tick_mark
    // (ruler_paint.cpp, pxview-render) — same dependency-injection pattern as
    // draw_logic_tick_mark above.
    QColor fore = _foreColor.isValid()
                      ? _foreColor
                      : QWidget::palette().color(QWidget::foregroundRole());
    pv::view::paint_osc_tick_mark(p, _view, rect(), fore,
        _min_period, _cur_prefix);
}

void Ruler::draw_hover_mark(QPainter &p)
{
    const double x = _view.hover_point().x();

	if (x == -1 || _grabbed_marker)
		return;

    QColor fore = _foreColor.isValid()
                      ? _foreColor
                      : QWidget::palette().color(QWidget::foregroundRole());
    p.setPen(fore);
    p.setBrush(fore);

	const int b = height() - 1;
    for (int i = 0; i < HoverArrowSize; i++)
        for (int j = -i; j <= i; j++)
            p.drawPoint(x-j, b-i);
}

void Ruler::draw_cursor_sel(QPainter &p)
{
    if (_cursor_sel_x == -1)
        return;

    p.setPen(QPen(Qt::NoPen));
    p.setBrush(View::Blue);

    const QPoint pos = QPoint(_view.hover_point().x(), _view.hover_point().y());
    if (in_cursor_sel_rect(pos) == 0)
        p.setBrush(View::Orange);

    const int y = height();
    const QRectF selRect = get_cursor_sel_rect(0);
    const QPointF del_points[] = {
        QPointF(_cursor_sel_x + CursorSelWidth / 2, (selRect.top() + CursorSelWidth / 2)),
        QPointF((selRect.left() + selRect.right()) / 2, selRect.top()),
        selRect.topLeft(),
        selRect.bottomLeft(),
        QPointF((selRect.left() + selRect.right()) / 2, selRect.bottom())
    };
    const QPointF points[] = {
        QPointF(_cursor_sel_x, y),
        selRect.bottomLeft(),
        selRect.topLeft(),
        selRect.topRight(),
        selRect.bottomRight()
    };
    p.drawPolygon((_cursor_go_visible ? del_points : points), countof(points));

    auto &cursor_list = _view.get_cursorList();

    if (!cursor_list.empty()) {
        int index = 1;

        for (auto &curosr : cursor_list) {
            const QRectF cursorRect = get_cursor_sel_rect(index);
            p.setPen(QPen(Qt::black, 1, Qt::DotLine));
            p.drawLine(cursorRect.left(), cursorRect.top() + 3,
                       cursorRect.left(), cursorRect.bottom() - 3);
            p.setPen(QPen(Qt::NoPen));

            if (in_cursor_sel_rect(pos) == index)
                p.setBrush(View::Orange);
            else
                p.setBrush(curosr->get_color());

            p.drawRect(cursorRect);
            p.setPen(Qt::black);
            p.drawText(cursorRect, Qt::AlignCenter | Qt::AlignVCenter, QString::number(index));
            index++;
        }
    }
}

int Ruler::in_cursor_sel_rect(QPointF pos)
{
    if (_cursor_sel_x == -1)
        return -1;

    auto &cursor_list = _view.get_cursorList();

    for (unsigned int i = 0; i < cursor_list.size() + 1; i++) {
        const QRectF cursorRect = get_cursor_sel_rect(i);
        if (cursorRect.contains(pos))
            return i;
    }

    return -1;
}

QRectF Ruler::get_cursor_sel_rect(int index)
{
    if (_cursor_sel_x == -1)
        return QRectF(-1, -1, 0, 0);
    const int y = height();
    return QRectF(_cursor_sel_x - (0.5 - index) * CursorSelWidth,
                  y - 1.3 * CursorSelWidth,
                  CursorSelWidth, CursorSelWidth);
}

void Ruler::hover_point_changed()
{
	update();
}

double Ruler::get_min_period()
{
    return _min_period / MinPeriodScale;
}

} // namespace view
} // namespace pv
