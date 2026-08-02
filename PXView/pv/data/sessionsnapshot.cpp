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

#include "sessionsnapshot.h"
#include "analogsnapshot.h"
#include "dsosnapshot.h"
#include "logicsnapshot.h"
#include "snapshot.h"
#include "signalmodel.h"
#include "lissajousmodel.h"
#include "triggerconfig.h"

#include <libsigrok/libsigrok.h>
#include <cstdlib>
#include <cstring>

namespace pv {
namespace data {

SessionSnapshot::SessionSnapshot()
    : _samplerate(0), _samplelimits(0), _trig_pos(0) {}

SessionSnapshot::~SessionSnapshot() {}

std::vector<std::shared_ptr<SignalModel>> &SessionSnapshot::get_signal_models() {
  return _signal_models;
}

std::vector<std::shared_ptr<DecoderStack>> &
SessionSnapshot::get_decoder_stacks(SessionDocument *doc) {
  (void)doc; // A SessionSnapshot always returns its own stacks.
  return _decoder_stacks;
}

std::vector<std::shared_ptr<SpectrumStack>> &SessionSnapshot::get_spectrum_stacks() {
  return _spectrum_stacks;
}

std::shared_ptr<MathStack> SessionSnapshot::get_math_stack() { return _math_stack; }

LissajousModel *SessionSnapshot::get_lissajous_model() {
  return _lissajous_model;
}

uint64_t SessionSnapshot::cur_snap_samplerate() {
  if (_samplerate == 0)
    return 1;
  return _samplerate;
}

uint64_t SessionSnapshot::cur_samplelimits() { return _samplelimits; }

double SessionSnapshot::cur_sampletime() {
  if (_samplerate == 0)
    return 0;
  return _samplelimits * 1.0 / _samplerate;
}

double SessionSnapshot::cur_snap_sampletime() {
  if (_samplerate == 0)
    return 0;
  return _samplelimits * 1.0 / _samplerate;
}

data::LogicSnapshot *SessionSnapshot::get_logic_snapshot() { return &_logic; }

data::AnalogSnapshot *SessionSnapshot::get_analog_snapshot() {
  return &_analog;
}

data::DsoSnapshot *SessionSnapshot::get_dso_snapshot() { return &_dso; }

data::Snapshot *SessionSnapshot::get_snapshot(int type) {
  if (type == SR_CHANNEL_LOGIC)
    return &_logic;
  else if (type == SR_CHANNEL_ANALOG)
    return &_analog;
  else if (type == SR_CHANNEL_DSO)
    return &_dso;
  else
    return nullptr;
}

uint64_t SessionSnapshot::get_trigger_pos() { return _trig_pos; }

// Task D6: DataSource facade/business stubs. SessionSnapshot is a frozen
// data snapshot, not the live session — these return defaults. Only
// SigSession performs real work for these operations.

double SessionSnapshot::cur_view_time() { return cur_sampletime(); }

int SessionSnapshot::get_map_zoom() { return 0; }

double SessionSnapshot::get_logic_data_view_time() { return 0.0; }

const TriggerConfig& SessionSnapshot::trigger_config() const {
    static TriggerConfig default_cfg;
    return default_cfg;
}

bool SessionSnapshot::is_repeating() { return false; }

bool SessionSnapshot::is_running_status() { return false; }

bool SessionSnapshot::is_instant() { return false; }

bool SessionSnapshot::have_view_data() { return true; }

bool SessionSnapshot::is_working() { return false; }

bool SessionSnapshot::add_decoder(srd_decoder *const, bool, DecoderStatus *,
                                  std::list<decode::Decoder *> &,
                                  std::shared_ptr<DecoderStack> &,
                                  SessionDocument *) {
    return false;
}

void SessionSnapshot::remove_decoder_by_key_handel(void *, SessionDocument *) {}

void SessionSnapshot::rst_decoder_by_key_handel(void *, SessionDocument *) {}

void SessionSnapshot::clear_all_decoder(bool) {}

void SessionSnapshot::start_all_decode_tasks() {}

void SessionSnapshot::update_dso_data_scale() {}

void SessionSnapshot::set_samplerate(uint64_t rate) {
  _samplerate = rate;
  if (rate > 0) {
    _logic.set_samplerate(rate);
    _analog.set_samplerate(rate);
    _dso.set_samplerate(rate);
  }
}

void SessionSnapshot::set_samplelimits(uint64_t limits) {
  _samplelimits = limits;
}

void SessionSnapshot::set_trigger_pos(uint64_t pos) { _trig_pos = pos; }

void SessionSnapshot::copy_from_logic(LogicSnapshot *src) {
  if (!src || src->empty())
    return;

  _logic.copy_from(*src);
}

void SessionSnapshot::copy_from_analog(AnalogSnapshot *src) {
  if (!src || src->empty())
    return;

  _analog.copy_from(*src);
}

void SessionSnapshot::copy_from_dso(DsoSnapshot *src) {
  if (!src || src->empty())
    return;

  _dso.copy_from(*src);
}

bool SessionSnapshot::load_from_file(const QString &file_name) {
  (void)file_name;
  return false;
}

} // namespace data
} // namespace pv
