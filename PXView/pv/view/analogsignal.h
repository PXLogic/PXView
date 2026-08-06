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


#ifndef PXVIEW_PV_ANALOGSIGNAL_H
#define PXVIEW_PV_ANALOGSIGNAL_H

#include "signal.h"

#include <memory>

namespace pv {

class SigSession;

namespace data {
class AnalogSnapshot;
class SignalModel;
class DataSource;
}

namespace view {

//when device is data acquisition model, to draw signal trace
//created by SigSession
class AnalogSignal : public Signal
{
    Q_OBJECT

private:
	static const QColor SignalColours[4];
	static const float EnvelopeThreshold;
    static const int NumSpanY = 5;
    static const int NumMiniSpanY = 5;
    static const int NumSpanX = 10;
    static const int HoverPointSize = 2;
    static const uint8_t DefaultBits = 8;

    // Safe narrow-cast: this is an AnalogSignal.
    AnalogSignal* as_analog() override { return this; }
    const AnalogSignal* as_analog() const override { return this; }
    void accept(TraceVisitor& v) override { v.visit(*this); }
    void accept(ConstTraceVisitor& v) const override { v.visit(*this); }

public:
    AnalogSignal(data::AnalogSnapshot *data,
                 std::shared_ptr<data::SignalModel> model,
                 data::DataSource *data_source);

    AnalogSignal(view::AnalogSignal* s,
                 data::AnalogSnapshot *data,
                 std::shared_ptr<data::SignalModel> model,
                 data::DataSource *data_source);

	virtual ~AnalogSignal();

    static QColor getSignalColor(int index);

    AnalogSignal* clone() const override;

    inline void set_scale(int height){
        _scale = height / (_ref_max - _ref_min);
    }

    inline float get_scale(){
        return _scale;
    }

    inline int get_bits(){
        return _bits;
    }

    inline double get_ref_min(){
        return _ref_min;
    }

    inline double get_ref_max(){
        return _ref_max;
    }

    inline data::AnalogSnapshot* data(){
        return _data;
    }

    void set_data(data::AnalogSnapshot *data);

    /// Signal override: extracts AnalogSnapshot from DataSource
    void set_data_from_source(data::DataSource *source) override;
    /// Signal override: sets _data to nullptr
    void clear_data() override;

    int get_hw_offset();
    int commit_settings();

    bool measure(const QPointF &p);
    bool get_hover(uint64_t &index, QPointF &p, double &value);
    QPointF get_point(uint64_t index, float &value);
    QString get_voltage(double v, int p, bool scaled = false);

    /**
     * Probe options
     **/
    uint64_t get_vdiv();
    uint8_t get_acCoupling();
    bool get_mapDefault();
    QString get_mapUnit();
    double get_mapMin();
    double get_mapMax();
    uint64_t get_factor();
    
    void set_zero_vpos(int pos);
    int get_zero_vpos();
    void set_zero_ratio(double ratio);
    double get_zero_ratio();

    inline int get_zero_offset(){
        return _zero_offset;
    }
    
    int ratio2value(double ratio);
    int ratio2pos(double ratio);
    double value2ratio(int value);
    double pos2ratio(int pos);

    /**
     * Event
     **/
    void resize();

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

private:
    void paint_trace(QPainter &p,
                     const pv::data::AnalogSnapshot *snapshot,
                     int zeroY, const int start_pixel,
                     const uint64_t start_index, const int64_t sample_count,
                     const double samples_per_pixel, const int order,
                     const float top, const float bottom, const int width);

    void paint_per_pixel(QPainter &p,
                         const pv::data::AnalogSnapshot *snapshot,
                         int zeroY, const int left, const int right,
                         const uint64_t start_index,
                         const int64_t sample_count,
                         const double samples_per_pixel, const int order,
                         const float top, const float bottom, const int width);

    void paint_envelope(QPainter &p,
                        const pv::data::AnalogSnapshot *snapshot,
                        int zeroY, const int start_pixel,
                        const uint64_t start_index, const int64_t sample_count,
                        const double samples_per_pixel, const int order,
                        const float top, const float bottom, const int width);

    void paint_hover_measure(QPainter &p, QColor fore, QColor back);

private:
	pv::data::AnalogSnapshot *_data;

    std::unique_ptr<QRectF[]> _rects;
    // 性能修复: paint_trace 复用成员缓冲，避免每帧 new/delete QPointF[]。
    // 与 _rects 同生命周期管理 (构造 nullptr / 析构+resize 释放 / 按需扩容)。
    std::unique_ptr<QPointF[]> _points;
    int64_t _points_cap;

	float _scale;
    // float 电压数据的缩放（参考 PulseView scale_ = div_height / resolution）。
    // ADC 整数路径用 _scale + hw_offset；float 路径用 _float_scale 直接缩放电压值。
    double _zero_vrate;
    int _zero_offset;
    int _cached_hw_offset;
    int _bits;
    double _ref_min;
    double _ref_max;

    bool _hover_en;
	uint64_t _hover_index;
	QPointF _hover_point;
	float _hover_value;
	float _float_scale;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_ANALOGSIGNAL_H
