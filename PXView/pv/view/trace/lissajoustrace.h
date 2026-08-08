/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
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


#ifndef PXVIEW_PV_LISSAJOUSTRACE_H
#define PXVIEW_PV_LISSAJOUSTRACE_H

#include "pv/view/trace/trace.h"
  
namespace pv {

namespace data {
class DsoSnapshot;
}

namespace view {

//when device is oscilloscope mode, it can use to draw Lissajous graph
//created by SigSession
class LissajousTrace : public Trace
{
    Q_OBJECT

private:
    static const int DIV_NUM = 10; 

public:
    LissajousTrace(bool enable, pv::data::DsoSnapshot *data,
                   int xIndex, int yIndex, int percent);

    virtual ~LissajousTrace();

    inline bool enabled(){
        return _enable;
    }

    // Safe narrow-cast: this is a LissajousTrace.
    LissajousTrace* as_lissajous() override { return this; }
    const LissajousTrace* as_lissajous() const override { return this; }
    void accept(TraceVisitor& v) override { v.visit(*this); }
    void accept(ConstTraceVisitor& v) const override { v.visit(*this); }

    inline void set_enable(bool enable){
        _enable = enable;
    }

    inline int xIndex(){
        return _xIndex;
    }

    inline void set_xIndex(int xIndex) {
        _xIndex = xIndex;
    }

    inline int yIndex(){
        return _yIndex;
    }

    inline void set_yIndex(int yIndex) {
        _yIndex = yIndex;
    }

    inline int percent(){
        return _percent;
    }

    inline void set_percent(int percent) {
        _percent = percent;
    }

    inline pv::data::DsoSnapshot* get_data(){
        return _data;
    }

    inline void set_data(pv::data::DsoSnapshot* data){
        _data = data;
    }

    inline int rows_size(){
        return 0;
    }

    /**
     * Paints the background layer of the trace with a QPainter
     * @param p the QPainter to paint into.
     * @param left the x-coordinate of the left edge of the signal
     * @param right the x-coordinate of the right edge of the signal
     **/
    void paint_back(QPainter &p, int left, int right, QColor fore, QColor back);

	/**
	 * Paints the signal with a QPainter
	 * @param p the QPainter to paint into.
	 * @param left the x-coordinate of the left edge of the signal.
	 * @param right the x-coordinate of the right edge of the signal.
	 **/
    void paint_mid(QPainter &p, int left, int right, QColor fore, QColor back);

    /**
     * Paints the signal with a QPainter
     * @param p the QPainter to paint into.
     * @param left the x-coordinate of the left edge of the signal.
     * @param right the x-coordinate of the right edge of the signal.
     **/
    void paint_fore(QPainter &p, int left, int right, QColor fore, QColor back);

    void paint_label(QPainter &p, int right, const QPoint pt, QColor fore);

private:
    pv::data::DsoSnapshot *_data;

    bool _enable;
    int _xIndex;
    int _yIndex;
    int _percent;
    QRect _border;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_LISSAJOUSTRACE_H
