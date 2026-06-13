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

#ifndef PXVIEW_PV_DATA_DATASOURCE_H
#define PXVIEW_PV_DATA_DATASOURCE_H

#include <stdint.h>
#include <vector>

namespace pv {

namespace view {
class Signal;
class DecodeTrace;
class SpectrumTrace;
class LissajousTrace;
class MathTrace;
}

namespace data {

class LogicSnapshot;
class AnalogSnapshot;
class DsoSnapshot;
class DecoderModel;
class Snapshot;

class DataSource
{
public:
    virtual ~DataSource() {}

    virtual std::vector<view::Signal*>& get_signals() = 0;
    virtual std::vector<view::DecodeTrace*>& get_decode_signals() = 0;
    virtual std::vector<view::SpectrumTrace*>& get_spectrum_traces() = 0;
    virtual view::LissajousTrace* get_lissajous_trace() = 0;
    virtual view::MathTrace* get_math_trace() = 0;
    virtual uint64_t cur_snap_samplerate() = 0;
    virtual uint64_t cur_samplelimits() = 0;
    virtual double cur_sampletime() = 0;
    virtual double cur_snap_sampletime() = 0;
    virtual data::LogicSnapshot* get_logic_snapshot() = 0;
    virtual data::AnalogSnapshot* get_analog_snapshot() = 0;
    virtual data::DsoSnapshot* get_dso_snapshot() = 0;
    virtual data::Snapshot* get_snapshot(int type) = 0;
    virtual data::DecoderModel* get_decoder_model() = 0;
    virtual uint64_t get_trigger_pos() = 0;
};

} // namespace data
} // namespace pv

#endif
