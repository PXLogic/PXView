/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
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


#include <cassert>
#include <cmath>
#include <typeinfo>

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QVariant>

#include "pv/view/trace/trace.h"
#include "pv/session/sigsession.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/config/appconfig.h"
#include "pv/core/appcontrol.h"
#include "pv/ui/dockfonts.h"


namespace pv {
namespace view {

// const QColor Trace::PROBE_COLORS[8] = {
//     QColor(0x50, 0x50, 0x50),	// Black
//     QColor(0x8F, 0x52, 0x02),	// Brown
//     QColor(0xCC, 0x00, 0x00),	// Red
//     QColor(0xF5, 0x79, 0x00),	// Orange
//     QColor(0xED, 0xD4, 0x00),	// Yellow
//     QColor(0x73, 0xD2, 0x16),	// Green
//     QColor(0x34, 0x65, 0xA4),	// Blue
//     QColor(0x75, 0x50, 0x7B),	// Violet
// };

const QColor Trace::PROBE_COLORS[8] = {
    
    QColor(0x75, 0x50, 0x7B),	// Violet
    QColor(0x34, 0x65, 0xA4),	// Blue
    QColor(0x73, 0xD2, 0x16),	// Green
    QColor(0xED, 0xD4, 0x00),	// Yellow
    QColor(0xF5, 0x79, 0x00),	// Orange
    QColor(0xCC, 0x00, 0x00),	// Red
    QColor(0x8F, 0x52, 0x02),	// Brown
    QColor(0x50, 0x50, 0x50),	// Black

};

const int Trace::LabelHitPadding = 2;

Trace::Trace(QString name, uint16_t index, int type) :
    _view(nullptr),
	_name(name),
    _v_offset(INT_MAX),
    _type(type),
    _sec_index(0),
    _totalHeight(30),
    _ownHeight(-1),
    _typeWidth(SquareNum)
{
    _index_list.push_back(index);
    _view_index = -1;
}

Trace::Trace(QString name, std::list<int> index_list, int type, int sec_index) :
    _view(nullptr),
    _name(name),
    _v_offset(INT_MAX),
    _type(type),
    _index_list(index_list),
    _sec_index(sec_index),
    _totalHeight(30),
    _ownHeight(-1),
    _typeWidth(SquareNum)
{
    _view_index = -1;
}

Trace::Trace(const Trace &t) :
    _view(t._view),
    _name(t._name),
    _colour(t._colour),
    _v_offset(t._v_offset),
    _type(t._type),
    _index_list(t._index_list),
    _sec_index(t._sec_index),
    _old_v_offset(t._old_v_offset),
    _totalHeight(t._totalHeight),
    _ownHeight(t._ownHeight),
    _typeWidth(t._typeWidth),
    _visible(t._visible),
    _text_size(t._text_size)
{
    // 双层坐标必须成对复制：只复制 layout 值而让 visual 留在默认哨兵，
    // 会让副本停在"未布局"位置 —— 渲染时整条波形消失。
    _visual_v_offset = t._visual_v_offset;
    _view_index = -1;
}


int Trace::get_name_width()
{
    QFont font = theme_font_trace_label();
    QFontMetrics fm(font);

    return fm.boundingRect(get_name()).width();
}

void Trace::set_name(QString name)
{
	_name = name;
}

int Trace::get_index()
{
    if(_index_list.size() == 0){
        pxv_warn("Trace::get_index: _index_list is empty, returning 0");
        return 0;
    }

    return _index_list.front();
}

void Trace::set_index_list(const std::list<int> &index_list)
{
    if (index_list.size() == 0){
        pxv_warn("Trace::set_index_list: index_list is empty, ignoring");
        return;
    }

    _index_list = index_list;
}

int Trace::get_zero_vpos()
{
    // Must return the same coordinate space as get_y() (both feed the
    // hit-test rects in pt_in_rect), otherwise the clickable region and the
    // painted region drift apart during a repositioning animation — the user
    // would have to click where the channel *will* be, not where it is.
    return _visual_v_offset;
}

void Trace::resize()
{
}

void Trace::set_view(pv::view::IRenderView *view)
{
	assert(view);
	if (_view == view)
		return; // idempotent — prevent double-connecting resize signal
	_view = view;
    // QML migration Task 3.1: the resize hookup moved behind the widget-free
    // IRenderView interface (string-based connect inside View::subscribe_resize
    // — Trace::resize is a private slot, same effective connection as the
    // former connect(_view, &View::resize, this, &Trace::resize)).
    if (_view)
        _view->subscribe_resize(this);
}

void Trace::set_viewport(pv::view::IRenderViewport *viewport)
{
    assert(viewport);
    _viewport = viewport;
}
 
void Trace::paint_prepare()
{
    assert(_view);
    _view->set_trig_hoff(0);
}

void Trace::paint_back(QPainter &p, int left, int right, QColor fore, QColor back, const PaintContext &ctx)
{
    (void)back;
    (void)ctx;

    fore.setAlpha(IRenderView::BackAlpha);
    QPen pen(fore);
    pen.setStyle(Qt::DotLine);
    p.setPen(pen);
    const double sigY = get_y();
    p.drawLine(left, static_cast<int>(sigY), right, static_cast<int>(sigY));
}

void Trace::paint_mid(QPainter &p, int left, int right, QColor fore, QColor back, const PaintContext &ctx)
{
	(void)p;
	(void)left;
	(void)right;
    (void)fore;
    (void)back;
    (void)ctx;
}

void Trace::paint_fore(QPainter &p, int left, int right, QColor fore, QColor back, const PaintContext &ctx)
{
	(void)p;
	(void)left;
	(void)right;
	(void)fore;
	(void)back;
    (void)ctx;
}

void Trace::paint_label(QPainter &p, int right, const QPoint pt, QColor fore)
{
    if (_type == SR_CHANNEL_FFT && !enabled())
        return;

    if (_type != SR_CHANNEL_DSO && _type != SR_CHANNEL_MATH && !visible())
        return;

    if (_type == SR_CHANNEL_DSO) {
        // Hot path debug logging removed for performance
    }

    compute_text_size(p);
    const int y = get_y();

    const QRectF color_rect = get_rect("color", y, right);

    // Paint the ColorButton
    QColor foreBack = fore;
    p.setPen(Qt::transparent);
    QColor color_set_rect = AppConfig::Instance().GetThemeColor(
        QString("@logic-channel-%1").arg(*_index_list.begin() % 8));
    if (!color_set_rect.isValid())
        color_set_rect = PROBE_COLORS[*_index_list.begin() % countof(PROBE_COLORS)];
    p.setBrush(enabled() ? (_colour.isValid() ? _colour : color_set_rect) : foreBack);

    int radius = 3; 
    p.drawRoundedRect(color_rect, radius, radius);
    
    if (_type == SR_CHANNEL_DSO ||
        _type == SR_CHANNEL_MATH) {
        p.setPen(enabled() ?  Qt::white: foreBack);
        p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, _name);
    }

    if (right <= get_leftWidth() + get_rightWidth()) {
        if (enabled()) {
            p.setPen(Qt::white);
            if (_type == SR_CHANNEL_GROUP)
                p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, "G");
            else if (_type == SR_CHANNEL_DECODER)
                p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, "D");
            else if (_type == SR_CHANNEL_FFT)
                p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, "F");
            else if (_type == SR_CHANNEL_MATH)
                p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, "M");
            else
                p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, QString::number(_index_list.front()));
        }
        // DSO controls are drawn in paint_type_options, don't return early
        if (_type != SR_CHANNEL_DSO && _type != SR_CHANNEL_MATH)
            return;
    }

    const QRectF name_rect  = get_rect("name",  y, right);
    const QRectF label_rect = get_rect("label", get_zero_vpos(), right);

    if (_type != SR_CHANNEL_DSO) {
        // Paint the signal name
        p.setPen(enabled() ?  fore: foreBack);
        p.drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter, _name);
    }

    // Paint the trigButton
    paint_type_options(p, right, pt, fore);

    // Paint the label
    if (enabled()) {
        const QPointF points[] = {
            label_rect.topLeft(),
            label_rect.topRight(),
            QPointF(right, get_zero_vpos()),
            label_rect.bottomRight(),
            label_rect.bottomLeft()
        };

        p.setPen(Qt::transparent);
        if (_type == SR_CHANNEL_DSO ||
            _type == SR_CHANNEL_FFT ||
            _type == SR_CHANNEL_ANALOG ||
            _type == SR_CHANNEL_MATH) {
            // fallback 与色块按钮/LOGIC 三角一致:_colour 无效时(如加载
            // 默认配置 colour="default" → QColor("default") invalid)用
            // @logic-channel-N 主题色,避免 invalid QColor 渲染成黑色
            QColor color = AppConfig::Instance().GetThemeColor(
                QString("@logic-channel-%1").arg(*_index_list.begin() % 8));
            if (!color.isValid())
                color = PROBE_COLORS[*_index_list.begin() % countof(PROBE_COLORS)];
            p.setBrush(enabled() ? (_colour.isValid() ? _colour : color) : foreBack);
            p.drawPolygon(points, countof(points));
        } else {
            QColor color = AppConfig::Instance().GetThemeColor(
                QString("@logic-channel-%1").arg(*_index_list.begin() % 8));
            if (!color.isValid())
                color = PROBE_COLORS[*_index_list.begin() % countof(PROBE_COLORS)];
            p.setBrush(enabled() ? (_colour.isValid() ? _colour : color) : foreBack);
            p.drawPolygon(points, countof(points));
        }

        p.setPen(Qt::white);
        const QPointF arrow_points[] = {
            QPoint(static_cast<int>(label_rect.right()), static_cast<int>(label_rect.center().y())),
            QPoint(static_cast<int>(label_rect.right()), static_cast<int>(label_rect.center().y()-1)),
            QPoint(static_cast<int>(label_rect.right()), static_cast<int>(label_rect.center().y()+1)),
            QPoint(static_cast<int>(label_rect.right()), static_cast<int>(label_rect.center().y()-2)),
            QPoint(static_cast<int>(label_rect.right()), static_cast<int>(label_rect.center().y()+2)),
            QPoint(static_cast<int>(label_rect.right()), static_cast<int>(label_rect.center().y()-3)),
            QPoint(static_cast<int>(label_rect.right()), static_cast<int>(label_rect.center().y()+3)),
            QPoint(static_cast<int>(label_rect.right()), static_cast<int>(label_rect.center().y()-4)),
            QPoint(static_cast<int>(label_rect.right()), static_cast<int>(label_rect.center().y()+4)),
            QPoint(static_cast<int>(label_rect.right()-1), static_cast<int>(label_rect.center().y()-3)),
            QPoint(static_cast<int>(label_rect.right()-1), static_cast<int>(label_rect.center().y()+3)),
            QPoint(static_cast<int>(label_rect.right()+1), static_cast<int>(label_rect.center().y()-3)),
            QPoint(static_cast<int>(label_rect.right()+1), static_cast<int>(label_rect.center().y()+3)),
            QPoint(static_cast<int>(label_rect.right()-1), static_cast<int>(label_rect.center().y()-2)),
            QPoint(static_cast<int>(label_rect.right()-1), static_cast<int>(label_rect.center().y()+2)),
            QPoint(static_cast<int>(label_rect.right()+1), static_cast<int>(label_rect.center().y()-2)),
            QPoint(static_cast<int>(label_rect.right()+1), static_cast<int>(label_rect.center().y()+2)),
            QPoint(static_cast<int>(label_rect.right()-2), static_cast<int>(label_rect.center().y()-2)),
            QPoint(static_cast<int>(label_rect.right()-2), static_cast<int>(label_rect.center().y()+2)),
            QPoint(static_cast<int>(label_rect.right()+2), static_cast<int>(label_rect.center().y()-2)),
            QPoint(static_cast<int>(label_rect.right()+2), static_cast<int>(label_rect.center().y()+2)),
        };
        if (label_rect.contains(pt) || selected())
            p.drawPoints(arrow_points, countof(arrow_points));

        // Paint the text
        p.setPen(Qt::white);
        if (_type == SR_CHANNEL_GROUP)
            p.drawText(label_rect, Qt::AlignCenter | Qt::AlignVCenter, "G");
        else if (_type == SR_CHANNEL_DECODER)
            p.drawText(label_rect, Qt::AlignCenter | Qt::AlignVCenter, "D");
        else if (_type == SR_CHANNEL_FFT)
            p.drawText(label_rect, Qt::AlignCenter | Qt::AlignVCenter, "F");
        else if (_type == SR_CHANNEL_MATH)
            p.drawText(label_rect, Qt::AlignCenter | Qt::AlignVCenter, "M");
        else
            p.drawText(label_rect, Qt::AlignCenter | Qt::AlignVCenter, QString::number(_index_list.front()));

        
        if (_type == SR_CHANNEL_GROUP)
            p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, "G");
        else if (_type == SR_CHANNEL_DECODER)
            p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, "D");
        else if (_type == SR_CHANNEL_FFT)
            p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, "F");
        else if (_type == SR_CHANNEL_MATH)
            p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, "M");
        else
            p.drawText(color_rect, Qt::AlignCenter | Qt::AlignVCenter, QString::number(_index_list.front()));
    }
}

void Trace::paint_type_options(QPainter &p, int right, const QPoint pt, QColor fore)
{
    (void)p;
    (void)right;
    (void)pt;
    (void)fore;
}

bool Trace::mouse_double_click(int right, const QPoint pt)
{
    (void)right;
    (void)pt;
    return false;
}

bool Trace::mouse_press(int right, const QPoint pt)
{
    (void)right;
    (void)pt;
    return false;
}

bool Trace::mouse_wheel(int right, const QPoint pt, const int shift)
{
    (void)right;
    (void)pt;
    (void)shift;
    return false;
}

int Trace::pt_in_rect(int y, int right, const QPoint &point)
{
    const QRectF color = get_rect("color", y, right);
    const QRectF name  = get_rect("name", y, right);
    const QRectF label = get_rect("label", get_zero_vpos(), right);

    if (color.contains(point) && enabled())
        return COLOR;
    else if (name.contains(point) && enabled())
        return NAME;
    else if (label.contains(point) && enabled())
        return LABEL;
    else
        return 0;
}

void Trace::compute_text_size(QPainter &p)
{
    _text_size = QSize(
        static_cast<int>(p.boundingRect(QRectF(), 0, "99").width()),
        static_cast<int>(p.boundingRect(QRectF(), 0, "99").height()));
}

QRect Trace::get_view_rect()
{
    // Rebind model: a trace may be queried before layout_time_signals()
    // assigns _view (e.g. during tab-close detach of a borrowing tab), or
    // after its view was torn down. Report an empty rect instead of
    // asserting — paint paths guard with the INT_MAX _v_offset sentinel.
    // The error log names the concrete trace type so the calling path can
    // be identified from the app log.
    if (!_view) {
        pxv_err("Trace::get_view_rect: _view not set (type=%s)",
                typeid(*this).name());
        return QRect();
    }
    return QRect(0, 0, _view->scroll_viewport_width(),
                 _view->scroll_viewport_height());
}

QColor Trace::get_text_colour()
{
	return (_colour.lightness() > 64) ? Qt::black : Qt::white;
}

void Trace::on_text_changed(const QString &text)
{
	set_name(text);
	text_changed();
}

void Trace::on_colour_changed(const QColor &colour)
{
	set_colour(colour);
	colour_changed();
}

int Trace::rows_size()
{
    return 1;
}

QRectF Trace::get_rect(const char *s, int y, int right)
{
    const QSizeF color_size(SquareWidth, SquareWidth);
    //const QSizeF color_size(SquareWidth, SquareWidth);
   // const QSizeF name_size(right - get_leftWidth() - get_rightWidth(), SquareWidth);
    const QSizeF name_size(right - get_leftWidth() - get_rightWidth(), SquareWidth);
    const QSizeF label_size(SquareWidth, SquareWidth);

    if (!strcmp(s, "name"))
        return QRectF(
            get_leftWidth(),
            y - name_size.height() / 2,
            name_size.width(), name_size.height());
    else if (!strcmp(s, "label"))
        return QRectF(
            right - 1.5f * label_size.width(),
            y - label_size.height() / 2,
            label_size.width(), label_size.height());
    else if (!strcmp(s, "color"))
        return QRectF(
            12,
            y - color_size.height() / 2,
            color_size.width(), color_size.height());
            //right - 2, color_size.height());
    else
        return QRectF(
            12,
            y - SquareWidth / 2.0,
            SquareWidth, SquareWidth);
}

// ============================================================================
// 双层 y 坐标：拖动跟手 + 落位动画
// ----------------------------------------------------------------------------
// 语义与 PulseView TraceTreeItem 的 force_to_v_offset /
// animate_to_layout_v_offset 同构（pv/views/trace/tracetreeitem.cpp:73-96）：
//   - force_to_v_offset()  ：拖动中跟手，硬设两个坐标，无动画延迟
//   - animate_to_layout_v_offset()：100ms OutQuad 把 visual 推向 layout，
//     这就是"被挤开的通道滑过去让位"的来源
// ============================================================================

void Trace::force_to_v_offset(int v_offset) {
  stop_v_offset_animation();
  _v_offset = _visual_v_offset = v_offset;
}

void Trace::animate_to_layout_v_offset() {
  // 已在目标位置：无需动画（这也是绝大多数帧的情况，避免无谓的动画对象分配）。
  if (_visual_v_offset == _v_offset)
    return;

  // INT_MAX 是"尚未布局"哨兵。从/到哨兵值做插值会产生天文数字的中间帧，
  // 且哨兵值本身不是有效坐标 —— 直接硬设，不做动画。
  if (_visual_v_offset == INT_MAX || _v_offset == INT_MAX) {
    _visual_v_offset = _v_offset;
    return;
  }

  // 懒创建动画对象（以 this 为父，随 Trace 析构）。
  if (!_v_offset_animation) {
    auto *anim = new QPropertyAnimation(this, "visual_v_offset", this);
    anim->setDuration(kVOffsetAnimationMs);
    anim->setEasingCurve(QEasingCurve::OutQuad);
    // 每帧把变化通知出去：View 侧据此设置 need_update 并触发重绘。
    // 不用 QPropertyAnimation 默认的自动 update()，因为绘制链路由
    // Viewport/Header 驱动，必须走它们的刷新入口。
    connect(anim, &QPropertyAnimation::valueChanged, this,
            [this](const QVariant &) { on_visual_v_offset_changed(); });
    _v_offset_animation = anim;
  }

  // 目标未变且正在跑：不要重启，否则会不断从当前位置重新计 100ms，
  // 拖动中频繁调用会让动画永远到不了终点（PulseView 同款守卫）。
  if (_v_offset_animation->endValue().toInt() == _v_offset &&
      _v_offset_animation->state() == QAbstractAnimation::Running)
    return;

  _v_offset_animation->stop();
  _v_offset_animation->setStartValue(_visual_v_offset);
  _v_offset_animation->setEndValue(_v_offset);
  _v_offset_animation->start();
}

void Trace::stop_v_offset_animation() {
  if (_v_offset_animation &&
      _v_offset_animation->state() != QAbstractAnimation::Stopped)
    _v_offset_animation->stop();
}

bool Trace::is_v_offset_animating() const {
  return _v_offset_animation &&
         _v_offset_animation->state() == QAbstractAnimation::Running;
}

void Trace::on_visual_v_offset_changed() {
  // 动画每帧到达这里：通知所属 View 重绘。_view 可能为空（未绑定视图的
  // Trace，或视图正在拆除），此时静默跳过 —— 动画本身仍会把值推到终点。
  if (_view)
    _view->request_animation_repaint();
}

} // namespace view
} // namespace pv
