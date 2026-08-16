/*
 * This file is part of the PulseView project.
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

#ifndef PXVIEW_PV_DATA_MATHSTACK_H
#define PXVIEW_PV_DATA_MATHSTACK_H

#include "pv/data/model/signaldata.h"
#include "pv/data/isignal_model_source.h"

#include <list>

#include <optional> 
  

#include <QObject>
#include <QString>

namespace pv {

namespace data {

class DsoSnapshot;
class SignalModel;

class MathStack : public QObject, public SignalData
{
    Q_OBJECT

public:
    enum math_state {
        Init,
        Stopped,
        Running
    };

    enum MathType {
        MATH_ADD,
        MATH_SUB,
        MATH_MUL,
        MATH_DIV,
    };

    struct EnvelopeSample
    {
        double min;
        double max;
    };

    struct EnvelopeSection
    {
        uint64_t start;
        unsigned int scale;
        uint64_t length;
        EnvelopeSample *samples;
    };

private:
    struct Envelope
    {
        uint64_t length;
        uint64_t data_length;
        EnvelopeSample *samples;
    };

private:
    static const unsigned int ScaleStepCount = 10;
    static const int EnvelopeScalePower;
    static const int EnvelopeScaleFactor;
    static const float LogEnvelopeScaleFactor;
    static const uint64_t EnvelopeDataUnit;

    static const int vDialValueCount = 19;
    static const uint64_t vDialValue[vDialValueCount];
    static const int vDialUnitCount = 3;
    static const QString vDialAddUnit[vDialUnitCount];
    static const QString vDialMulUnit[vDialUnitCount];
    static const QString vDialDivUnit[vDialUnitCount];

public:
    MathStack(pv::data::ISignalModelSource *_source,
              int ch1_index,
              int ch2_index, MathType type);
    virtual ~MathStack();
    void clear();
    void init();
    void free_envelop();
    void realloc(uint64_t num);

    MathType get_type();
    uint64_t get_sample_num();

    int ch1_index() const { return _ch1_index; }
    int ch2_index() const { return _ch2_index; }

    void enable_envelope(bool enable);

    uint64_t default_vDialValue();
    uint64_t default_factor();

    // dslDial 相邻档位步进 — View 层 (MathTrace) 构造 dslDial 需要.
    static constexpr uint64_t vDialValueStep = 1000;
    // 纯数据 vDial 配置 (依赖倒置: Core 层不得实例化 view::dslDial — QWidget).
    // View 层 (MathTrace) 消费此数据自行创建控件. (d93e20a5 重放)
    void get_vdial_data(QVector<uint64_t> &vValue,
                        QVector<QString> &vUnit) const;
    QString get_unit(int level);
    double get_math_scale();

    const double *get_math(uint64_t start);
    void get_math_envelope_section(EnvelopeSection &s,
        uint64_t start, uint64_t end, float min_length);

    void calc_math(uint64_t mathFactor);
    void reallocate_envelope(Envelope &e);
    void append_to_envelope_level(bool header);

signals:

private:
    pv::data::ISignalModelSource  *_source;
    int              _ch1_index;
    int              _ch2_index;

    MathType _type;
    uint64_t _sample_num;
    uint64_t _total_sample_num;
    math_state _math_state;

    struct Envelope _envelope_level[ScaleStepCount];
    std::vector<double> _math;

    bool _envelope_en;
    bool _envelope_done;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_MATHSTACK_H
