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


#ifndef PXVIEW_PV_LOGICSIGNAL_H
#define PXVIEW_PV_LOGICSIGNAL_H

#include "pv/view/signal/signal.h"

#include <vector>
#include <memory>

namespace pv {

class SigSession;

namespace data {
    class LogicSnapshot;
    class SignalModel;
    class DataSource;
}

namespace view {

//when device is logic analyzer mode, to draw logic signal trace
//created by SigSession
class LogicSignal : public Signal
{
    Q_OBJECT

private:
	static const float Oversampling;

    static const int StateHeight;
    static const int StateRound;

public:
    static const int TogMaxScale = 10;

    // Safe narrow-cast: this is a LogicSignal.
    LogicSignal* as_logic() override { return this; }
    const LogicSignal* as_logic() const override { return this; }
    void accept(TraceVisitor& v) override { v.visit(*this); }
    void accept(ConstTraceVisitor& v) const override { v.visit(*this); }

public:
    enum LogicSetRegions{
        NONTRIG = 0,
        POSTRIG,
        HIGTRIG,
        NEGTRIG,
        LOWTRIG,
        EDGTRIG,
    };

public:
    LogicSignal(data::LogicSnapshot *data,
                std::shared_ptr<data::SignalModel> model,
                data::DataSource *data_source);

    LogicSignal(view::LogicSignal *s,
                data::LogicSnapshot *data,
                std::shared_ptr<data::SignalModel> model,
                data::DataSource *data_source);

	virtual ~LogicSignal();

    LogicSignal* clone() const override;

    inline data::LogicSnapshot* data(){
        return _data;
    }

    // P1: shared_ptr to the logic snapshot (keeps it alive during async render).
    inline std::shared_ptr<data::LogicSnapshot> data_ref() const {
        return _data_ref;
    }

    void set_data(data::LogicSnapshot* data);

    /// Signal override: extracts LogicSnapshot from DataSource
    void set_data_from_source(data::DataSource *source) override;
    /// Signal override: sets _data to nullptr
    void clear_data() override;

    inline LogicSetRegions get_trig(){
        return _trig;
    }

    void set_trig(int trig);

    bool commit_trig();

	/**
	 * Paints the signal with a QPainter
	 * @param p the QPainter to paint into.
	 * @param left the x-coordinate of the left edge of the signal.
	 * @param right the x-coordinate of the right edge of the signal.
	 **/
    void paint_mid(QPainter &p, int left, int right, QColor fore, QColor back, const PaintContext &ctx) override;

    bool measure(const QPointF &p, uint64_t &index0, uint64_t &index1, uint64_t &index2);

    bool is_by_edge(const QPointF &p, uint64_t &index, int radius);

    bool edge(const QPointF &p, uint64_t &index, int radius);

    bool edges(const QPointF &p, uint64_t start, uint64_t &rising, uint64_t &falling);

    bool edges(uint64_t end, uint64_t start, uint64_t &rising, uint64_t &falling);

    bool mouse_press(int right, const QPoint pt);

    QRectF get_rect(LogicSetRegions type, int y, int right);

    void paint_mark(QPainter &p, int xstart, int xend, int type, int edge_dir = 0);

    void paint_mid_align_sample(QPainter &p, int left, int right, QColor fore, QColor back, uint64_t end_align_sample, const PaintContext &ctx);

protected:
    void paint_type_options(QPainter &p, int right, const QPoint pt, QColor fore);

private:
	void paint_caps(QPainter &p, QLineF *const lines,
        std::vector< std::pair<uint64_t, bool> > &edges,
		bool level, double samples_per_pixel, double pixels_offset,
		float x_offset, float y_offset);

    void paint_mid_align(QPainter &p, int left, int right, QColor fore, QColor back, uint64_t end_align_sample, const PaintContext &ctx);

private:
	std::shared_ptr<pv::data::LogicSnapshot> _data_ref; // keeps snapshot alive (prevents use-after-free)
    pv::data::LogicSnapshot* _data;
    std::vector< std::pair<uint16_t, bool> > _cur_edges;
    std::vector<std::pair<bool, bool>> _cur_pulses;
    LogicSetRegions _trig;
    uint64_t    _paint_align_sample_count;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_LOGICSIGNAL_H
