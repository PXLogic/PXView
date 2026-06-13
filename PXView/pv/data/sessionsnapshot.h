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

#ifndef PXVIEW_PV_DATA_SESSIONSNAPSHOT_H
#define PXVIEW_PV_DATA_SESSIONSNAPSHOT_H

#include <stdint.h>
#include <vector>
#include <QString>
#include <QDateTime>
#include "datasource.h"
#include "logicsnapshot.h"
#include "analogsnapshot.h"
#include "dsosnapshot.h"

namespace pv {

namespace view {
class Signal;
class DecodeTrace;
class SpectrumTrace;
class LissajousTrace;
class MathTrace;
}

namespace data {

class DecoderModel;

class SessionSnapshot : public DataSource
{
public:
    SessionSnapshot();
    ~SessionSnapshot();

    std::vector<view::Signal*>& get_signals() override;
    std::vector<view::DecodeTrace*>& get_decode_signals() override;
    std::vector<view::SpectrumTrace*>& get_spectrum_traces() override;
    view::LissajousTrace* get_lissajous_trace() override;
    view::MathTrace* get_math_trace() override;
    uint64_t cur_snap_samplerate() override;
    uint64_t cur_samplelimits() override;
    double cur_sampletime() override;
    double cur_snap_sampletime() override;
    data::LogicSnapshot* get_logic_snapshot() override;
    data::AnalogSnapshot* get_analog_snapshot() override;
    data::DsoSnapshot* get_dso_snapshot() override;
    data::Snapshot* get_snapshot(int type) override;
    data::DecoderModel* get_decoder_model() override;
    uint64_t get_trigger_pos() override;

    void set_samplerate(uint64_t rate);
    void set_samplelimits(uint64_t limits);
    void set_trigger_pos(uint64_t pos);

    void copy_from_logic(data::LogicSnapshot *src);
    void copy_from_analog(data::AnalogSnapshot *src);
    void copy_from_dso(data::DsoSnapshot *src);

    inline LogicSnapshot* get_logic() { return &_logic; }
    inline AnalogSnapshot* get_analog() { return &_analog; }
    inline DsoSnapshot* get_dso() { return &_dso; }

    inline QDateTime timestamp() const { return _timestamp; }
    inline void set_timestamp(const QDateTime &ts) { _timestamp = ts; }

    inline QString file_path() const { return _file_path; }
    inline void set_file_path(const QString &path) { _file_path = path; }

    bool load_from_file(const QString &file_name);

private:
    uint64_t _samplerate;
    uint64_t _samplelimits;
    uint64_t _trig_pos;

    LogicSnapshot _logic;
    AnalogSnapshot _analog;
    DsoSnapshot _dso;

    std::vector<view::Signal*> _signals;
    std::vector<view::DecodeTrace*> _decode_traces;
    std::vector<view::SpectrumTrace*> _spectrum_traces;
    view::LissajousTrace *_lissajous_trace;
    view::MathTrace *_math_trace;
    DecoderModel *_decoder_model;

    QDateTime _timestamp;
    QString _file_path;
};

} // namespace data
} // namespace pv

#endif
