/*
 * This file is part of the PXView project.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pv/data/document/sessiondocument.h"
#include "pv/data/stack/decoderstack.h"  // DecoderStack (IsRunning/stop_decode_work complete type)
#include "pv/base/log.h"
#include "pv/data/stack/lissajousmodel.h"
#include "pv/data/stack/mathstack.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/stack/spectrumstack.h"
#include <QDebug>
#include <libsigrok/libsigrok.h>

namespace pv {
namespace data {

class DecoderStack;

SessionDocument::SessionDocument(data::IDeviceConfigPort *device_port)
    : _samplerate(0), _samplelimits(0), _trigger_pos(0),
      _signal_config_store(std::make_unique<SignalConfigStore>(device_port)) {}

SessionDocument::~SessionDocument() {}

LogicSnapshot *SessionDocument::get_logic_snapshot() {
  return get_active_logic();
}

AnalogSnapshot *SessionDocument::get_analog_snapshot() {
  return get_active_analog();
}

DsoSnapshot *SessionDocument::get_dso_snapshot() { return get_active_dso(); }

LogicSnapshot *SessionDocument::get_active_logic() { return _logic.get(); }

AnalogSnapshot *SessionDocument::get_active_analog() { return _analog.get(); }

DsoSnapshot *SessionDocument::get_active_dso() { return _dso.get(); }

// Zero-copy: share the shared_ptr (increment ref count) instead of deep-copying.
// The OLD shared_ptr is moved to _pending_* (deferred release) to keep the old
// snapshot alive until the View rebinds its raw pointers (clear_pending_release).
void SessionDocument::share_from_logic(std::shared_ptr<LogicSnapshot> src) {
  _pending_logic = _logic;  // Keep old alive (releases previous pending if any)
  _logic = std::move(src);
  // Self-contained: stamp the snapshot with the document samplerate so the
  // renderer (LogicSignal::paint_mid_align) reads _data->samplerate() correctly
  // without any external pass-through. The document always knows its samplerate
  // (set via set_samplerate before sharing), so the snapshot becomes complete here.
  if (_logic)
    _logic->set_samplerate((double)_samplerate);
}

void SessionDocument::share_from_analog(std::shared_ptr<AnalogSnapshot> src) {
  _pending_analog = _analog;
  _analog = std::move(src);
}

void SessionDocument::share_from_dso(std::shared_ptr<DsoSnapshot> src) {
  _pending_dso = _dso;
  _dso = std::move(src);
}

void SessionDocument::clear_pending_release() {
  _pending_logic.reset();
  _pending_analog.reset();
  _pending_dso.reset();
}

void SessionDocument::set_samplerate(uint64_t rate) { _samplerate = rate; }

uint64_t SessionDocument::get_samplerate() const { return _samplerate; }

void SessionDocument::set_samplelimits(uint64_t limits) {
  _samplelimits = limits;
}

uint64_t SessionDocument::get_samplelimits() const { return _samplelimits; }

void SessionDocument::set_trigger_pos(uint64_t pos) { _trigger_pos = pos; }

uint64_t SessionDocument::get_trigger_pos() { return _trigger_pos; }

double SessionDocument::get_sampletime() const {
  if (_samplerate == 0)
    return 0;
  return _samplelimits * 1.0 / _samplerate;
}

bool SessionDocument::has_data() {
  return (_logic && !_logic->empty()) ||
         (_analog && !_analog->empty()) ||
         (_dso && !_dso->empty());
}

bool SessionDocument::empty() { return !has_data(); }

void SessionDocument::clear() {
  // Reset shared_ptrs — releases the document's reference to the snapshot data.
  // If SessionData still holds a shared_ptr to the same snapshot, the data
  // stays alive (ref count > 0). This replaces the old in-place _logic.clear()
  // which would have cleared data shared with SessionData.
  _logic.reset();
  _analog.reset();
  _dso.reset();
  _pending_logic.reset();
  _pending_analog.reset();
  _pending_dso.reset();
  _samplerate = 0;
  _samplelimits = 0;
  _trigger_pos = 0;
}

std::vector<std::shared_ptr<DecoderStack>> &
SessionDocument::get_decoder_stacks(SessionDocument *doc) {
  (void)doc; // A SessionDocument always returns its own stacks.
  return _decoder_stacks;
}

void SessionDocument::add_decoder_stack(std::shared_ptr<DecoderStack> stack) {
  if (stack)
    _decoder_stacks.push_back(stack);
}

void SessionDocument::remove_decoder_stack(std::shared_ptr<DecoderStack> stack) {
  auto it = std::find(_decoder_stacks.begin(), _decoder_stacks.end(), stack);
  if (it != _decoder_stacks.end())
    _decoder_stacks.erase(it);
}

// 幂等 + 并发安全的清空。设备切换时 MCP worker 线程（set_device→
// clear_active_document_decoders）与 GUI 主线程（CurrentDeviceChangePrev→
// del_all_protocol→clear_all_decoder）会同时清同一份 _decoder_stacks：直接
// .clear() 是双线程数据竞争 → 迭代器/对象悬垂 → Windows 堆损坏/SIGSEGV。
// 统一加锁串行化：先清空的在锁内销毁所有栈，后到的看到空向量即无操作。
void SessionDocument::clear_decoder_stacks() {
  std::lock_guard<std::mutex> lock(_stacks_mutex);
  for (auto &s : _decoder_stacks)
    if (s && s->IsRunning())
      s->stop_decode_work();
  _decoder_stacks.clear();
}

bool SessionDocument::decoder_stacks_empty() const {
  std::lock_guard<std::mutex> lock(_stacks_mutex);
  return _decoder_stacks.empty();
}

// DataSource pure-virtual overrides. SessionDocument does NOT own live copies
// of these — SigSession holds the single source of truth. These always return
// empty/null (matching the prior behavior when the dead fields existed but were
// never populated). See view.cpp comment near line 2625.
// (purify-architecture-concepts Task 10) get_decoder_model() was removed from
// the DataSource interface — DecoderModel now lives in pv::view.

std::vector<std::shared_ptr<SignalModel>> &SessionDocument::get_signal_models() {
  static std::vector<std::shared_ptr<SignalModel>> empty;
  return empty;
}

std::vector<std::shared_ptr<SpectrumStack>> &SessionDocument::get_spectrum_stacks() {
  static std::vector<std::shared_ptr<SpectrumStack>> empty;
  return empty;
}

std::shared_ptr<MathStack> SessionDocument::get_math_stack() { return nullptr; }

LissajousModel *SessionDocument::get_lissajous_model() { return nullptr; }

uint64_t SessionDocument::cur_snap_samplerate() { return _samplerate; }

uint64_t SessionDocument::cur_samplelimits() { return _samplelimits; }

double SessionDocument::cur_sampletime() {
  return _samplerate > 0 ? (_samplelimits * 1.0 / _samplerate) : 0.0;
}

double SessionDocument::cur_snap_sampletime() {
  return _samplerate > 0 ? (_samplelimits * 1.0 / _samplerate) : 0.0;
}

data::Snapshot *SessionDocument::get_snapshot(int type) {
  if (type == SR_CHANNEL_LOGIC)
    return get_active_logic();
  else if (type == SR_CHANNEL_ANALOG)
    return get_active_analog();
  else if (type == SR_CHANNEL_DSO)
    return get_active_dso();
  else
    return nullptr;
}

// Task D6: DataSource facade/business stubs. SessionDocument is a pure data
// container — it does not own the live session state or decoder lifecycle.
// These return sensible defaults; only SigSession performs real work. The
// View layer calls these through DataSource* so it never reaches into the
// SigSession facade for data/business operations.

double SessionDocument::cur_view_time() { return cur_sampletime(); }

int SessionDocument::get_map_zoom() { return 0; }

double SessionDocument::get_logic_data_view_time() { return 0.0; }

bool SessionDocument::is_repeating() { return false; }

bool SessionDocument::is_instant() { return false; }

bool SessionDocument::have_view_data() { return has_data(); }

bool SessionDocument::is_working() { return false; }

bool SessionDocument::add_decoder(srd_decoder *const, bool, DecoderStatus *,
                                  std::list<decode::Decoder *> &,
                                  std::shared_ptr<DecoderStack> &,
                                  SessionDocument *) {
  return false;
}

void SessionDocument::remove_decoder_by_key_handel(void *, SessionDocument *) {}

void SessionDocument::rst_decoder_by_key_handel(void *, SessionDocument *) {}

void SessionDocument::clear_all_decoder(bool) {}

void SessionDocument::start_all_decode_tasks() {}

void SessionDocument::update_dso_data_scale() {}

// Wrap SignalConfigStore's serialization and merge in triggerConfig from
// _trigger_config to keep .pxc format unchanged (trigger_config remains a
// SessionDocument-owned field).
QJsonObject SessionDocument::signal_config_to_json() const {
  QJsonObject obj = _signal_config_store->signal_config_to_json();
  obj["triggerConfig"] = _trigger_config.to_json();
  return obj;
}

void SessionDocument::signal_config_from_json(const QJsonObject &obj) {
  _signal_config_store->signal_config_from_json(obj);
  if (obj.contains("triggerConfig")) {
    // Task 6: from_json 改为静态工厂（返回新对象），用赋值替代原成员调用。
    _trigger_config = data::TriggerConfig::from_json(
        obj["triggerConfig"].toObject());
  }
}

void SessionDocument::set_trigger_config(const data::TriggerConfig &cfg) {
  _trigger_config = cfg;
}

} // namespace data
} // namespace pv
