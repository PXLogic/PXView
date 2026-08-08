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

#include "pv/data/snapshot/analogsnapshot.h"
#include "pv/data/datasource.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/stack/lissajousmodel.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/model/signalconfigstore.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/triggerconfig.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <map>
#include <memory>
#include <cstdint>
#include <vector>

namespace pv {

class SigSession;
class TabContext;

namespace data {

class DecoderStack;
class SpectrumStack;
class MathStack;

// SessionDocument is now a pure data container. Signal/pending config and
// DeviceAgent interaction have been extracted to SignalConfigStore
// (accessed via signal_config_store()). trigger_config remains here as a
// SessionDocument-owned field. The SigSession* is injected so SignalConfigStore
// can reach DeviceAgent via _session->get_device() without SessionDocument
// itself depending on DeviceAgent.
class SessionDocument : public DataSource {
public:
  explicit SessionDocument(SigSession *session);
  ~SessionDocument();

  LogicSnapshot *get_logic_snapshot() override;
std::shared_ptr<LogicSnapshot> get_logic_snapshot_shared() override { return _logic; }
  AnalogSnapshot *get_analog_snapshot() override;
  std::shared_ptr<AnalogSnapshot> get_analog_snapshot_shared() override { return _analog; }
  DsoSnapshot *get_dso_snapshot() override;
  std::shared_ptr<DsoSnapshot> get_dso_snapshot_shared() override { return _dso; }

  LogicSnapshot *get_active_logic();
  AnalogSnapshot *get_active_analog();
  DsoSnapshot *get_active_dso();

  // Zero-copy ownership sharing: assigns the shared_ptr (increments ref count)
  // instead of deep-copying the snapshot data. Replaces the old copy_from_*
  // methods. After sharing, both SessionData and SessionDocument point to the
  // same underlying snapshot. When SessionData::clear() resets its shared_ptr,
  // the snapshot stays alive because SessionDocument still holds a reference.
  void share_from_logic(std::shared_ptr<LogicSnapshot> src);
  void share_from_analog(std::shared_ptr<AnalogSnapshot> src);
  void share_from_dso(std::shared_ptr<DsoSnapshot> src);

  // Release the deferred (previous) shared_ptrs. Call this AFTER the View has
  // rebound its signal raw pointers to the new snapshots (e.g. after
  // set_data_document completes). Until this is called, the old snapshots
  // stay alive (ref count > 0), preventing use-after-free on the View's raw
  // pointers.
  void clear_pending_release();

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

  std::vector<std::shared_ptr<DecoderStack>> &
  get_decoder_stacks(SessionDocument *doc = nullptr) override;
  void add_decoder_stack(std::shared_ptr<DecoderStack> stack);
  void remove_decoder_stack(std::shared_ptr<DecoderStack> stack);
  // get_signal_models()/get_spectrum_stacks()/get_math_stack()/
  // get_lissajous_model() below are DataSource overrides returning
  // empty/null because SessionDocument never populated these fields (only
  // SigSession holds the live copies). See view.cpp comment near line 2625.
  // (purify-architecture-concepts Task 10) get_decoder_model() was removed
  // from the DataSource interface entirely — DecoderModel now lives in the
  // View layer (pv::view) and is owned by ProtocolDock.

  std::vector<std::shared_ptr<SignalModel>> &get_signal_models() override;
  std::vector<std::shared_ptr<SpectrumStack>> &get_spectrum_stacks() override;
  std::shared_ptr<MathStack> get_math_stack() override;
  LissajousModel *get_lissajous_model() override;
  uint64_t cur_snap_samplerate() override;
  uint64_t cur_samplelimits() override;
  double cur_sampletime() override;
  double cur_snap_sampletime() override;
  data::Snapshot *get_snapshot(int type) override;

  // Task D6: DataSource facade/business overrides — stubs returning
  // defaults because SessionDocument does not own the live session state
  // (SigSession is the single source of truth). Only SigSession performs
  // real work for these; the View layer routes through DataSource so it
  // does not reach into the SigSession facade directly.
  double cur_view_time() override;
  int get_map_zoom() override;
  double get_logic_data_view_time() override;
  bool is_repeating() override;
  bool is_running_status() override;
  bool is_instant() override;
  bool have_view_data() override;
  bool is_working() override;
  bool add_decoder(srd_decoder *const dec, bool silent, DecoderStatus *dstatus,
                   std::list<decode::Decoder *> &sub_decoders,
                   std::shared_ptr<DecoderStack> &out_stack,
                   SessionDocument *doc = nullptr) override;
  void remove_decoder_by_key_handel(void *handel,
                                    SessionDocument *doc = nullptr) override;
  void rst_decoder_by_key_handel(void *handel,
                                 SessionDocument *doc = nullptr) override;
  void clear_all_decoder(bool bUpdateView = true) override;
  void start_all_decode_tasks() override;
  void update_dso_data_scale() override;

  // --- Signal config forwarding (delegated to SignalConfigStore) ---
  // signal_config_to_json/from_json wrap the store's version and merge in
  // triggerConfig from _trigger_config to keep .pxc format unchanged.
  QJsonObject signal_config_to_json() const;
  void signal_config_from_json(const QJsonObject &obj);
  void save_signal_config(
      const std::vector<std::shared_ptr<SignalModel>> &signal_models = {},
      const std::map<int, ChannelLayoutState> &channel_layout = {},
      const std::map<int, std::string> &channel_colours = {}) {
    _signal_config_store->save_signal_config(signal_models, channel_layout,
                                             channel_colours);
  }
  void apply_signal_config() { _signal_config_store->apply_signal_config(); }
  void apply_pending_config() {
    _signal_config_store->apply_pending_config();
  }
  bool has_signal_config() const {
    return _signal_config_store->has_signal_config();
  }
  bool has_pending_config() const {
    return _signal_config_store->has_pending_config();
  }
  const SignalConfig &get_signal_config() const {
    return _signal_config_store->get_signal_config();
  }
  // For TabContext to restore trig_type after reload (replaces friend access).
  const std::vector<ChannelConfig> &get_channels() const {
    return _signal_config_store->get_channels();
  }
  // For TabContext to save pending config (replaces friend access).
  void set_pending_config(const SignalConfig &cfg) {
    _signal_config_store->set_pending_config(cfg);
  }
  SignalConfigStore *signal_config_store() {
    return _signal_config_store.get();
  }

  inline const data::TriggerConfig &trigger_config() const override {
    return _trigger_config;
  }
  inline data::TriggerConfig &trigger_config() { return _trigger_config; }
  void set_trigger_config(const data::TriggerConfig &cfg);

private:
  // shared_ptr members: enable zero-copy ownership sharing with SessionData.
  // clear() resets these to nullptr; get_active_*() returns nullptr when reset.
  // Callers that check has_data() first are safe — has_data() returns false
  // when all shared_ptrs are null or point to empty snapshots.
  std::shared_ptr<LogicSnapshot> _logic;
  std::shared_ptr<AnalogSnapshot> _analog;
  std::shared_ptr<DsoSnapshot> _dso;

  // Deferred-release: when share_from_* replaces _logic/_analog/_dso, the OLD
  // shared_ptrs are moved here (not immediately released). This keeps the old
  // snapshots alive until clear_pending_release() is called, which the View
  // does AFTER rebinding its raw signal pointers to the new snapshots. Without
  // this, a paint event between share_from_* and set_data_document would
  // dereference a dangling pointer (use-after-free).
  std::shared_ptr<LogicSnapshot> _pending_logic;
  std::shared_ptr<AnalogSnapshot> _pending_analog;
  std::shared_ptr<DsoSnapshot> _pending_dso;
  uint64_t _samplerate;
  uint64_t _samplelimits;
  uint64_t _trigger_pos;
  std::vector<std::shared_ptr<DecoderStack>> _decoder_stacks;
  // Dead storage removed (purify-architecture-concepts Task 1):
  // _decoder_model / _signal_models / _spectrum_stacks / _math_stack /
  // _lissajous_model were never populated (only clear()-ed) and shadowed the
  // live copies in SigSession. The DataSource get_* overrides above now return
  // empty/null literals. set_decoder_model (zero callers) was deleted.
  std::unique_ptr<SignalConfigStore> _signal_config_store;
  data::TriggerConfig _trigger_config;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_SESSIONDOCUMENT_H
