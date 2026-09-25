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

#ifndef PXVIEW_PV_VIEW_TRACE_H
#define PXVIEW_PV_VIEW_TRACE_H

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QRect>
#include <QString>
#include <cstdint>
#include "pv/view/iview_delegates.h"
#include "pv/view/trace/selectableitem.h"
#include "pv/view/trace/paint_context.h"
#include "pv/view/component/dsldial.h"
#include "pv/view/trace/trace_visitor.h"

class QFormLayout;
class QPropertyAnimation;

namespace pv {
namespace view {

class View;
class Viewport;
class LogicSignal;
class DsoSignal;
class AnalogSignal;
class DecodeTrace;
class SpectrumTrace;
class MathTrace;
class LissajousTrace;

//base class
class Trace : public SelectableItem
{
	Q_OBJECT

    // 双层 y 坐标的可动画属性（PulseView TraceTreeItem 同构设计）。
    // QPropertyAnimation 写这个属性即可驱动"让位"动画；QPropertyAnimation 属于
    // Qt6::Core，本类所在的 pxview-render 只链 Qt6::Gui，层约束不受影响。
    Q_PROPERTY(int visual_v_offset
        READ visual_v_offset
        WRITE set_visual_v_offset)

protected:
    static const int SquareNum = 5;
	static const int LabelHitPadding;

public:
    static const int SquareWidth = 20;
    static const int Margin = 3;
    static const int COLOR = 1;
    static const int NAME = 2;
    static const int LABEL = 8;

    static const QColor PROBE_COLORS[8];

protected:
    Trace(QString name, uint16_t index, int type);
    Trace(QString name, std::list<int> index_list, int type, int sec_index);

    /**
     * Copy constructor
     */
    Trace(const Trace &t);

public:
	/**
	 * Gets the name of this signal.
	 */
	inline QString get_name(){
        return _name;
    }

    virtual int get_name_width();

	/**
	 * Sets the name of the signal.
	 */
	virtual void set_name(QString name);

	/**
	 * Get the colour of the signal.
	 */
	inline QColor get_colour(){
        return _colour;
    }

	/**
	 * Set the colour of the signal.
	 */
	virtual void set_colour(QColor colour){
        _colour = colour;
    }

	/**
	 * Gets the vertical layout offset of this signal.
	 */
	inline int get_v_offset(){
        return _v_offset;
    }

	/**
	 * Sets the vertical layout offset of this signal.
	 *
	 * This is the **layout** coordinate (target position). The visual
	 * coordinate follows it immediately *unless* a repositioning animation
	 * is in flight — which is exactly the drag-reorder case, where the
	 * layout target must move ahead of the painted position so the
	 * animation has something to chase.
	 *
	 * Consequences to keep in mind:
	 *  - Normal layout passes (layout_time_signals, config restore, FFT
	 *    placement) call this outside any animation, so both coordinates
	 *    stay in lockstep and behaviour is unchanged.
	 *  - Code that wants an explicit animation must call this first, then
	 *    animate_to_layout_v_offset().
	 *  - Code that wants an instant, animation-free jump (drag follow) must
	 *    call force_to_v_offset() instead of this.
	 */
	inline void set_v_offset(int v_offset){
        _v_offset = v_offset;
        if (!is_v_offset_animating())
            _visual_v_offset = v_offset;
    }

    /**
     * Sets **only** the layout offset (target), leaving the visual offset
     * untouched. Used when staging an animation: the target must move before
     * animate_to_layout_v_offset() runs, otherwise set_v_offset() would
     * snap the visual coordinate and there would be nothing left to animate.
     */
    inline void set_v_offset_no_visual_sync(int v_offset) {
        _v_offset = v_offset;
    }

    // ======================================================================
    // 双层 y 坐标（PulseView TraceTreeItem 同构设计）
    // ----------------------------------------------------------------------
    // _v_offset        = layout 目标位置（"应该在哪儿"）—— 布局算法写它
    // _visual_v_offset = 实际绘制位置（"现在在哪儿"）—— 绘制读它
    //
    // 拖动/落位时布局只改 layout 值，再调 animate_to_layout_v_offset() 把
    // visual 值平滑推过去。这样"被拖项跟手 + 其余项滑动让位"才成立：
    // 若只有单层坐标，其余项只能瞬间跳变（本项目改造前的行为）。
    //
    // 约定：**布局 / 命中测试 / 排序** 读 get_v_offset()；
    //       **绘制** 读 get_y()（已切到 visual 值）。
    // ======================================================================

    /**
     * Gets the visual (actually painted) vertical offset.
     */
    inline int visual_v_offset() const {
        return _visual_v_offset;
    }

    /**
     * Sets the visual vertical offset. Normally driven by
     * QPropertyAnimation; call force_to_v_offset() instead to set both
     * coordinates at once without animating.
     */
    inline void set_visual_v_offset(int v_offset) {
        _visual_v_offset = v_offset;
    }

    /**
     * Hard-sets both the layout and the visual offset, cancelling any
     * running animation. Use while dragging (follow the cursor with no
     * animation delay) and to reset the two coordinates in lockstep.
     */
    void force_to_v_offset(int v_offset);

    /**
     * Animates the visual offset towards the current layout offset
     * (100ms, OutQuad). No-op when already there, or when an animation
     * towards the same target is already running.
     */
    void animate_to_layout_v_offset();

    /**
     * Stops a running visual-offset animation, leaving the visual offset
     * at its current (intermediate) value.
     */
    void stop_v_offset_animation();

    /**
     * @return true while the visual offset is still chasing the layout
     * offset (i.e. a repositioning animation is in flight).
     */
    bool is_v_offset_animating() const;

    /** Repositioning animation duration (ms). Mirrors PulseView. */
    static const int kVOffsetAnimationMs = 100;

    /**
     * Gets trace type
     */
    inline int get_type(){
        return _type;
    }

    /**
     * Index process
     */
    int get_index();

    inline std::list<int> get_index_list(){
        return _index_list;
    }

    void set_index_list(const std::list<int> &index_list);

    inline int get_sec_index(){
        return _sec_index;
    }

    inline void set_sec_index(int sec_index){
        _sec_index = sec_index;
    }

    /**
     * Gets the height of this signal.
     */
    inline int get_totalHeight(){
        return _totalHeight;
    }

    /**
     * Sets the height of this signal.
     */
    inline void set_totalHeight(int height){
         _totalHeight = height;
    }

    inline int get_own_height(){
        return _ownHeight;
    }

    inline void set_own_height(int height){
         _ownHeight = height;
    }

    inline bool visible() { return _visible; }
    inline void set_visible(bool v) { _visible = v; }

    inline int get_leftWidth(){
        // return SquareWidth/2 + Margin;
        return SquareWidth + Margin + 10;
    }

    inline int get_rightWidth(){
        // The 1.5 * SquareWidth term makes this a double expression; the
        // returned pixel width is deliberately truncated.
        return static_cast<int>(2 * Margin + _typeWidth * SquareWidth +
                                1.5 * SquareWidth);
    }

    inline int get_headerHeight(){
        return SquareWidth;
    }

    /**
     * Gets the old vertical layout offset of this signal.
     */
    inline int get_old_v_offset(){
        return _old_v_offset;
    }

    /**
     * Sets the old vertical layout offset of this signal.
     */
    inline void set_old_v_offset(int v_offset){
        _old_v_offset = v_offset;
    }

    virtual int get_zero_vpos();

	/**
	 * Returns true if this trace is hardware-enabled (Core-owned, mirrors
	 * SignalModel::_enabled / sr_channel->enabled). This is independent of
	 * UI visibility — use visible() to query whether the trace is shown on
	 * screen. The two concepts must not be conflated: a hardware-disabled
	 * channel may still be visible in the UI, and a hidden channel may
	 * still be hardware-enabled.
	 */
    virtual bool enabled() = 0;

	virtual void set_view(pv::view::IRenderView *view);

    inline pv::view::IRenderView* get_view(){
        return _view;
    }

    virtual void set_viewport(pv::view::IRenderViewport *viewport);

    inline pv::view::IRenderViewport* get_viewport(){
        return _viewport;
    }

    /**
     * Paints prepare
     **/
    virtual void paint_prepare();

	/**
	 * Paints the background layer of the trace with a QPainter
	 * @param p the QPainter to paint into.
	 * @param left the x-coordinate of the left edge of the signal
	 * @param right the x-coordinate of the right edge of the signal
	 **/
    virtual void paint_back(QPainter &p, int left, int right, QColor fore, QColor back, const PaintContext &ctx);

	/**
	 * Paints the mid-layer of the trace with a QPainter
	 * @param p the QPainter to paint into.
	 * @param left the x-coordinate of the left edge of the signal
	 * @param right the x-coordinate of the right edge of the signal
	 * @param ctx rendering context snapshot (scale, offset, etc.)
	 **/
    virtual void paint_mid(QPainter &p, int left, int right, QColor fore, QColor back, const PaintContext &ctx);

	/**
	 * Paints the foreground layer of the trace with a QPainter
	 * @param p the QPainter to paint into.
	 * @param left the x-coordinate of the left edge of the signal
	 * @param right the x-coordinate of the right edge of the signal
	 * @param ctx rendering context snapshot (scale, offset, etc.)
	 **/
    virtual void paint_fore(QPainter &p, int left, int right, QColor fore, QColor back, const PaintContext &ctx);

	/**
     * Paints the trace label.
	 * @param p the QPainter to paint into.
	 * @param right the x-coordinate of the right edge of the header
	 * 	area.
     * @param point the mouse point.
	 */
    virtual void paint_label(QPainter &p, int right, const QPoint pt, QColor fore);

	/**
	 * Gets the y-offset of the axis.
	 *
	 * Returns the **visual** offset (actually painted position), so that
	 * waveforms, labels and overlay layers all move together during a
	 * repositioning animation. Layout/hit-test code must use
	 * get_v_offset() / get_zero_vpos() instead.
	 */
	inline int get_y(){
        return _visual_v_offset;
    }

    /**
     * Determines if a point is in the header rect.
     * 1 - in color rect
     * 2 - in name rect
     * 3 - in posTrig rect
     * 4 - in higTrig rect
     * 5 - in negTrig rect
     * 6 - in lowTrig rect
     * 7 - in label rect
     * 0 - not
     * @param y the y-coordinate of the signal.
     * @param right the x-coordinate of the right edge of the header
     * 	area.
     * @param point the point to test.
     */
    int pt_in_rect(int y, int right,
        const QPoint &point);

    /**
     * Computes the outline rectangle of a label.
     * @param p the QPainter to lay out text with.
     * @param y the y-coordinate of the signal.
     * @param right the x-coordinate of the right edge of the header
     * 	area.
     * @return Returns the rectangle of the signal label.
     */
    QRectF get_rect(const char *s, int y, int right);

    /**
     * Safe narrow-cast methods — return this if the trace is of the
     * requested type, nullptr otherwise. Eliminates all dynamic_cast/
     * static_cast in consumer code.
     */
    virtual LogicSignal* as_logic() { return nullptr; }
    virtual DsoSignal* as_dso() { return nullptr; }
    virtual AnalogSignal* as_analog() { return nullptr; }
    virtual DecodeTrace* as_decode() { return nullptr; }
    virtual SpectrumTrace* as_spectrum() { return nullptr; }
    virtual MathTrace* as_math() { return nullptr; }
    virtual LissajousTrace* as_lissajous() { return nullptr; }

    /**
     * Const variants for read-only contexts.
     */
    virtual const LogicSignal* as_logic() const { return nullptr; }
    virtual const DsoSignal* as_dso() const { return nullptr; }
    virtual const AnalogSignal* as_analog() const { return nullptr; }
    virtual const DecodeTrace* as_decode() const { return nullptr; }
    virtual const SpectrumTrace* as_spectrum() const { return nullptr; }
    virtual const MathTrace* as_math() const { return nullptr; }
    virtual const LissajousTrace* as_lissajous() const { return nullptr; }

    /**
     * Visitor accept for multi-type dispatch.
     * Default implementation does nothing (base Trace is abstract).
     */
    virtual void accept(TraceVisitor&) {}
    virtual void accept(ConstTraceVisitor&) const {}

    virtual int rows_size();

    virtual QRect get_view_rect();

    virtual bool mouse_double_click(int right, const QPoint pt);

    virtual bool mouse_press(int right, const QPoint pt);

    virtual bool mouse_wheel(int right, const QPoint pt, const int shift);

    inline int signal_type(){
        return get_type();
    }

    inline void set_view_index(int index){
        _view_index = index;
    }

    inline int get_view_index(){
        return _view_index;
    }

protected:

	/**
	 * Gets the text colour.
	 * @remarks This colour is computed by comparing the lightness
	 * of the trace colour against a threshold to determine whether
	 * white or black would be more visible.
	 */
	QColor get_text_colour();

    /**
     * Paints optoins for different trace type.
     * @param p the QPainter to paint into.
     * @param right the x-coordinate of the right edge of the header
     * 	area.
     * @param point the mouse point.
     */
    virtual void paint_type_options(QPainter &p, int right, const QPoint pt, QColor fore);

private:

    /**
     * Computes an caches the size of the label text.
     */
    void compute_text_size(QPainter &p);

private slots:
	void on_text_changed(const QString &text);
	void on_colour_changed(const QColor &colour);
    virtual void resize();
    // 动画每帧回调：把"绘制位置变了"通知给所属 View。
    void on_visual_v_offset_changed();

signals:
	void visibility_changed();
	void text_changed();	
	void colour_changed();

protected:
	// Rendering services of the owning View/Viewport (widget-free interfaces,
	// QML migration Phase 3 Task 3.1). The concrete View/Viewport objects
	// implement these interfaces; GUI code keeps passing the concrete
	// pointers (implicit upcast at the call site).
	pv::view::IRenderView *_view;
    pv::view::IRenderViewport *_viewport;

	QString _name;
	QColor _colour;
	int _v_offset;
    // 实际绘制位置。动画期间与 _v_offset 不等；静止时两者相等。
    // 用 INT_MAX 作为"尚未布局"哨兵，与 _v_offset 保持一致。
    int _visual_v_offset = INT_MAX;
    int _type;
    std::list<int> _index_list;
    int _sec_index;
    int _old_v_offset;
    int _totalHeight;
    int _ownHeight;
    int _typeWidth;
    int _view_index;
    bool _visible = true;

    QSizeF _text_size;  

    // 驱动 _visual_v_offset 的动画。懒创建（首次 animate 时 new 并以 this 为父），
    // 避免为一堆从不做动画的 Trace 付构造开销。以 this 为父对象，故随 Trace 析构。
    QPropertyAnimation *_v_offset_animation = nullptr;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_TRACE_H
