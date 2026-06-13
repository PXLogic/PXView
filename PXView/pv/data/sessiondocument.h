/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PXVIEW_PV_DATA_SESSIONDOCUMENT_H
#define PXVIEW_PV_DATA_SESSIONDOCUMENT_H

#include <stdint.h>
#include <vector>
#include <map>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include "datasource.h"
#include "logicsnapshot.h"
#include "analogsnapshot.h"
#include "dsosnapshot.h"

class DeviceAgent;

namespace pv {
class TabContext;
namespace view { class Signal; }
namespace view { class DecodeTrace; }
namespace view { class SpectrumTrace; }
namespace view { class LissajousTrace; }
namespace view { class MathTrace; }

namespace data {

class DecoderStack;
class DecoderModel;

struct ChannelConfig {
    int index;
    bool enabled;
    uint64_t vdiv;
    int coupling;
    bool map_default;
    uint16_t hw_offset;
    uint16_t offset;
    uint16_t zero_offset;

    ChannelConfig() : index(0), enabled(false), vdiv(0), coupling(0),
                      map_default(true), hw_offset(0), offset(0), zero_offset(0) {}
};

struct SignalConfig {
    int work_mode;
    int operation_mode;
    int channel_mode;
    bool is_demo;
    QString demo_operation_mode;
    std::vector<ChannelConfig> channels;
    bool is_valid;

    SignalConfig() : work_mode(0), operation_mode(0), channel_mode(0),
                     is_demo(false), is_valid(false) {}
};

class SessionDocument : public DataSource
{
public:
    SessionDocument();
    ~SessionDocument();

    LogicSnapshot* get_logic_snapshot() override;
    AnalogSnapshot* get_analog_snapshot() override;
    DsoSnapshot* get_dso_snapshot() override;

    LogicSnapshot* get_active_logic();
    AnalogSnapshot* get_active_analog();
    DsoSnapshot* get_active_dso();
    void copy_from_logic(LogicSnapshot *src);
    void copy_from_analog(AnalogSnapshot *src);
    void copy_from_dso(DsoSnapshot *src);

    void set_samplerate(uint64_t rate);
    uint64_t get_samplerate() const;

    void set_samplelimits(uint64_t limits);
    uint64_t get_samplelimits() const;

    void set_trigger_pos(uint64_t pos);
    uint64_t get_trigger_pos() override;

    double get_sampletime() const;

    bool has_data();
    bool empty();

    void clear();

    std::vector<DecoderStack*>& get_decoder_stacks();
    void add_decoder_stack(DecoderStack *stack);
    void remove_decoder_stack(DecoderStack *stack);
    DecoderModel* get_decoder_model() override;
    void set_decoder_model(DecoderModel *model);

    std::vector<view::DecodeTrace*>& get_decode_traces();
    void add_decode_trace(view::DecodeTrace *trace);
    void remove_decode_trace(view::DecodeTrace *trace);

    std::vector<view::Signal*>& get_signals() override;
    std::vector<view::DecodeTrace*>& get_decode_signals() override;
    std::vector<view::SpectrumTrace*>& get_spectrum_traces() override;
    view::LissajousTrace* get_lissajous_trace() override;
    view::MathTrace* get_math_trace() override;
    uint64_t cur_snap_samplerate() override;
    uint64_t cur_samplelimits() override;
    double cur_sampletime() override;
    double cur_snap_sampletime() override;
    data::Snapshot* get_snapshot(int type) override;

    uint64_t _dock_sample_rate;
    uint64_t _dock_sample_limit;
    int _dock_collect_mode;
    std::map<uint16_t, QString> _dock_search_pattern;
    bool _dock_measure_fen_enabled;
    QJsonArray _dock_measure_dist_rows;
    QJsonArray _dock_measure_edge_rows;
    QJsonObject _dock_trigger_session;
    QJsonObject _dock_dso_trigger_session;
    QJsonObject _dock_device_options_session;
    QJsonObject _dock_signal_processing_session;
    QString _dock_protocol_search_text;
    QJsonArray _dock_protocol_expanded_states;

    QJsonObject signal_config_to_json() const;
    void signal_config_from_json(const QJsonObject &obj);
    void save_signal_config(DeviceAgent *agent);
    void apply_signal_config(DeviceAgent *agent);
    void apply_pending_config(DeviceAgent *agent);
    bool has_signal_config() const;
    bool has_pending_config() const;
    const SignalConfig& get_signal_config() const { return _signal_config; }

private:
    LogicSnapshot   _logic;
    AnalogSnapshot  _analog;
    DsoSnapshot     _dso;
    uint64_t        _samplerate;
    uint64_t        _samplelimits;
    uint64_t        _trigger_pos;
    std::vector<DecoderStack*> _decoder_stacks;
    DecoderModel    *_decoder_model;
    std::vector<view::DecodeTrace*> _decode_traces;
    std::vector<view::Signal*> _signals;
    std::vector<view::SpectrumTrace*> _spectrum_traces;
    SignalConfig _signal_config;
    SignalConfig _pending_device_config;

    friend class pv::TabContext;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_SESSIONDOCUMENT_H
