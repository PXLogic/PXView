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

#ifndef PXVIEW_PV_SIGSESSION_H
#define PXVIEW_PV_SIGSESSION_H

#include <QDateTime>
#include <QTimer>
#include <QString>
#include <atomic>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <cstdint>
#include <string>
#include <vector>
// Forward-declared snapshot types — full includes only needed in .cpp.
// This eliminates 3 heavy header chains (analogsnapshot.h → snapshot.h →
// <mutex>; dsosnapshot.h → snapshot.h; logicsnapshot.h → mmap_allocator.h
// → <atomic>,<thread>,<QString>) from 59 downstream includers.
namespace data { class AnalogSnapshot; class DsoSnapshot; class LogicSnapshot; }
#include "pv/data/datasource.h"
#include "pv/data/decoderanalogdata.h"
#include "pv/data/stack/mathstack.h"
#include "pv/data/document/sessiondata.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/triggerconfig.h"
#include "pv/session/deviceagent.h"
#include "pv/base/pxvdef.h"
#include "pv/base/eventobject.h"
#include "pv/interface/icallbacks.h"
#include "pv/interface/events.h"
#include "pv/core/eventbus.h"
#include "pv/core/capturemanager.h"
#include "pv/core/sessionstatecontext.h"
#include <libsigrok/libsigrok.h>

struct srd_decoder;
struct srd_channel;
class DecoderStatus;
using ds_lock_guard = std::lock_guard<std::mutex>;

// Forward declarations for upstream libsigrok types (now the sole libsigrok).
struct sr_context;

namespace pv {
namespace data {
class SignalData; class Snapshot; class LissajousModel; class SessionDocument;
class DecoderStack; class SpectrumStack;
namespace decode { class Decoder; }
} // namespace data
namespace core {
class FilterProcessor; class DecodeTaskManager; class DataFeedParser;
class DocumentRegistry; class CaptureManager;
} // namespace core

using namespace pv::data;

/**
 * SigSession — now a thin facade over SessionStateContext + 5 managers.
 *
 * modernize-core-layer-radical phase 1 broke the circular dependency between
 * SigSession and its 5 managers (CaptureManager / DocumentRegistry /
 * DecodeTaskManager / DataFeedParser / FilterProcessor). The managers
 * previously held a `SigSession*` and reached into SigSession's private
 * fields via friend declarations (263 direct `_session->_xxx` access sites).
 * Now the managers hold a `SessionStateContext*` and use accessor methods,
 * so SigSession no longer needs to friend the managers.
 *
 * SigSession retains ownership of: the EventBus (unique_ptr), the 5 manager
 * unique_ptrs, the SessionStateContext unique_ptr, the DeviceEventObject,
 * the IDecoderPannel pointer, the static _empty_decoder_stacks, and the
 * libsigrok opaque context (_srstd_ctx). All shared mutable state
 * (mutexes, signal models, device agent, view/capture data, atomic flags,
 * trigger config, etc.) lives on SessionStateContext and is accessed via
 * `_state->xxx()` accessors.
 *
 * Public API signatures are unchanged for backward compatibility with the
 * View/API layers. Inline method bodies that previously touched private
 * fields now forward to `_state->xxx()` accessors.
 */
class SigSession : public IDeviceAgentCallback, public pv::data::DataSource {
private:
  static constexpr float Oversampling = 2.0f;
  SigSession(SigSession &o);
public:
  // Timer cadence constants — re-exported from CaptureManager for backward
  // compatibility with View-layer callers (e.g. viewport.cpp uses
  // SigSession::FeedInterval). modernize-core-layer-radical phase 1 moved
  // the canonical definitions to CaptureManager since this manager owns the
  // timer cadence; SigSession now just re-exports them.
  static constexpr int RefreshTime = core::CaptureManager::RefreshTime;
  static constexpr int RepeatHoldDiv = core::CaptureManager::RepeatHoldDiv;
  static constexpr int FeedInterval = core::CaptureManager::FeedInterval;
  static constexpr int WaitShowTime = core::CaptureManager::WaitShowTime;

  // SESSION_ERROR_STATUS — re-exported from SessionStateContext for backward
  // compatibility (mainwindow.cpp uses SigSession::Hw_err etc.). The enum
  // definition lives on SessionStateContext; SigSession re-exports the type
  // via a using-alias and the enum values via static constexpr (using-
  // declarations would require SessionStateContext to be a base class, which
  // it is not — SigSession holds it via unique_ptr).
  using SESSION_ERROR_STATUS = core::SessionStateContext::SESSION_ERROR_STATUS;
  static constexpr SESSION_ERROR_STATUS No_err = core::SessionStateContext::No_err;
  static constexpr SESSION_ERROR_STATUS Hw_err = core::SessionStateContext::Hw_err;
  static constexpr SESSION_ERROR_STATUS Malloc_err = core::SessionStateContext::Malloc_err;
  static constexpr SESSION_ERROR_STATUS Test_timeout_err = core::SessionStateContext::Test_timeout_err;
  static constexpr SESSION_ERROR_STATUS Pkt_data_err = core::SessionStateContext::Pkt_data_err;
  static constexpr SESSION_ERROR_STATUS Data_overflow = core::SessionStateContext::Data_overflow;

  explicit SigSession();
  ~SigSession();
  DeviceAgent *get_device() { return &_state->device_agent(); }
  // Expose the SessionStateContext for the API layer to access
  // wait_for_capture_complete() (Phase 1 sync-wait bypass).
  core::SessionStateContext *get_state() { return _state.get(); }
  // Task D4: DataSource::device() override — exposes the DeviceAgent to the
  // View layer for device-level capability/probe queries that have no
  // SignalModel getter. Aliases get_device() (single device per session).
  DeviceAgent* device() override { return &_state->device_agent(); }
// Spec v2 Task 7: add_callback/remove_callback/set_callback removed (ISessionCallback abolished)
  bool init(); void uninit(); void Open(); void Close();
  bool set_default_device(); bool set_device(ds_device_handle dev_handle);
  bool set_file(QString name); void close_file(unsigned long long dev_handle) override;
  bool import_file(QString name);
  bool start_capture(bool instant = false, data::SessionDocument *owner = nullptr) override { return _capture_manager->start_capture(instant, owner); }
  bool stop_capture() override { return _capture_manager->stop_capture(); }
  /// Emergency fallback: force-release the capture state when the
  /// SessionStopped event was suppressed by the EventBus broadcast
  /// depth guard (caused by processEvents() re-entrancy). This sets
  /// _is_working=false and releases the CaptureOwnerGuard directly,
  /// mirroring what SigSession::on_event(SessionStopped) would have done.
  void force_release_capture_state();
  bool switch_work_mode(int mode) override;
  uint64_t cur_samplerate();
  uint64_t cur_snap_samplerate() override;
  uint64_t cur_samplelimits() override;
  double cur_sampletime() override;
  double cur_snap_sampletime() override;
  double cur_view_time() override;
  bool re_start() { if (_state->is_working()) stop_capture(); return start_capture(_capture_manager->is_instant()); }
  QDateTime get_session_time() { return _state->session_time(); }
  QDateTime get_trig_time() { return _state->trig_time(); }
  bool is_triged() { return _state->is_triged(); }
  uint64_t get_trigger_pos() override { return _state->view_data()->_trig_pos; }
  bool is_first_store_confirm() { return _capture_manager->is_first_store_confirm(); }
  bool get_capture_status(bool &triggered, int &progress) { return _capture_manager->get_capture_status(triggered, progress); }
  void clear_store_confirm_flag() { _capture_manager->clear_store_confirm_flag(); }
  std::vector<std::shared_ptr<data::SignalModel>> &get_signal_models() override;
  // TS-2 fix: thread-safe snapshot — locks signal_models_mutex() (shared)
  // and returns a value copy. Safe from any thread.
  std::vector<std::shared_ptr<data::SignalModel>> get_signal_models_snapshot() override;
  /// Mutex protecting the signal_models vector. Non-UI thread callers
  /// (decode thread, save/export thread) must hold a shared_lock before
  /// iterating; writers (init_signals) hold a unique_lock.
  std::shared_mutex &signal_models_mutex() { return _state->signal_models_mutex(); }
  bool add_decoder(srd_decoder *const dec, bool silent, DecoderStatus *dstatus, std::list<pv::data::decode::Decoder *> &sub_decoders, std::shared_ptr<data::DecoderStack> &out_stack, data::SessionDocument *doc = nullptr) override;
  int get_trace_index_by_key_handel(void *handel, data::SessionDocument *doc = nullptr);
  void remove_decoder(int index, data::SessionDocument *doc = nullptr);
  void remove_decoder_by_key_handel(void *handel, data::SessionDocument *doc = nullptr) override;
  std::vector<std::shared_ptr<data::DecoderStack>> &get_decoder_stacks(data::SessionDocument *doc = nullptr) override;
  void rst_decoder(int index, data::SessionDocument *doc = nullptr);
  void rst_decoder_by_key_handel(void *handel, data::SessionDocument *doc = nullptr) override;
  std::vector<std::shared_ptr<data::SpectrumStack>> &get_spectrum_stacks() override { return _state->spectrum_stacks(); }
  data::LissajousModel *get_lissajous_model() override { return _state->lissajous_model(); }
  std::shared_ptr<data::MathStack> get_math_stack() override { return _state->math_stack(); }
  uint16_t get_ch_num(int type);
  bool is_data_lock() { return _capture_manager->is_data_lock(); }
  void data_lock() { _capture_manager->data_lock(); }
  void data_unlock() { _capture_manager->data_unlock(); }
  void data_auto_lock(int lock) override { _capture_manager->data_auto_lock(lock); }
  void data_auto_unlock() { _capture_manager->data_auto_unlock(); }
  bool get_data_auto_lock() override { return _capture_manager->get_data_auto_lock(); }
  void spectrum_rebuild();
  void lissajous_rebuild(bool enable, int xindex, int yindex, double percent);
  void lissajous_disable();
  void math_rebuild(bool enable, int ch1_index, int ch2_index, data::MathStack::MathType type);
  bool trigd() override { return _state->trigger_flag(); }
  uint8_t trigd_ch() override { return _state->trigger_ch(); }
  data::Snapshot *get_snapshot(int type) override;
  data::LogicSnapshot *get_logic_snapshot() override;
std::shared_ptr<data::LogicSnapshot> get_logic_snapshot_shared() override;
  data::AnalogSnapshot *get_analog_snapshot() override;
  data::DsoSnapshot *get_dso_snapshot() override;
  // Task C1.5: DSO measurement computation via core::MeasureCalculator.
  // Overrides DataSource::get_measurements() to compute real measurement
  // values from the view_data() DsoSnapshot + signal_models. The View layer
  // (view::DsoMeasure::get_measure) and the MCP API (SessionService::
  // get_measurements) both call this so headless mode returns real data.
  std::vector<api::MeasurementValue> get_measurements(
      int channel_index = -1,
      int view_rect_height = 0) override;
  // Task C2.4: cursor position state forwarded to
  // SessionStateContext::cursor_registry(). The View layer reads/writes
  // through these so headless MCP clients see real cursor state without
  // a View binding. add_cursor returns the positional index of the new
  // entry, or -1 on failure.
  std::vector<core::CursorEntry> get_cursors() const override;
  int  add_cursor(uint64_t sample_position) override;
  bool remove_cursor(int index) override;
  bool set_cursor_position(int index, uint64_t sample_position) override;
  void clear_cursors() override;
  SESSION_ERROR_STATUS get_error() { return _state->error(); }
  void set_error(SESSION_ERROR_STATUS state) { _state->set_error(state); }
  void clear_error();
  uint64_t get_error_pattern() { return _state->error_pattern(); }
  double get_repeat_intvl() { return _capture_manager->get_repeat_intvl(); }
  void set_repeat_intvl(double interval) { _capture_manager->set_repeat_intvl(interval); }
  int get_repeat_hold() override { return _capture_manager->get_repeat_hold(); }
  void set_save_start(uint64_t start) { _state->set_save_start(start); }
  uint64_t get_save_start() { return _state->save_start(); }
  void set_save_end(uint64_t end) { _state->set_save_end(end); }
  uint64_t get_save_end() { return _state->save_end(); }
  void clear_all_decoder(bool bUpdateView = true) override;
  bool is_closed() { return _state->bClose(); }
  bool is_instant() override { return _capture_manager->is_instant(); }
  bool is_working() override { return _state->is_working() || _state->device_status() == ST_RUNNING; }
  bool is_init_status() { return _state->device_status() == ST_INIT; }
  bool is_running_status() override { return _state->device_status() == ST_RUNNING; }
  bool is_stopped_status() override { return _state->device_status() == ST_STOPPED; }
  void set_collect_mode(DEVICE_COLLECT_MODE m) { _capture_manager->set_collect_mode(m); }
  int get_collect_mode() { return _capture_manager->get_collect_mode(); }
  bool is_repeat_mode() override { return _capture_manager->is_repeat_mode(); }
  // UI pre-HOLD query used before the very first Repeat acquisition starts.
  bool repeat_analog_display_trigger_enabled_for_ui() {
    return repeat_analog_display_trigger_enabled();
  }
  uint64_t repeat_analog_trigger_ui_generation_for_ui() const {
    return _repeat_analog_trigger_ui_generation.load(std::memory_order_acquire);
  }
  bool is_single_mode() { return _capture_manager->is_single_mode(); }
  bool is_loop_mode() { return _capture_manager->is_loop_mode(); }
  bool is_realtime_refresh() override { return _capture_manager->is_realtime_refresh(); }
  bool is_repeating() override { return _capture_manager->is_repeating(); }
  void session_save() override { broadcast_async<interface::SaveRequested>({}); }
  void show_region(uint64_t start, uint64_t end, bool keep) { broadcast_async<interface::ShowRegion>({start, end, keep}); }
  void decode_done() override { broadcast_async<interface::DecodeDone>({}); }
  bool is_saving() { return _state->is_saving(); }
  void set_saving(bool flag) { _state->set_saving(flag); }
  DeviceEventObject *device_event_object() { return &_device_event; }
  void reload();
  void refresh(int holdtime) override { _capture_manager->refresh(holdtime); }
  void check_update() { _capture_manager->check_update(); }
  void set_map_zoom(int index) { _state->set_map_zoom(index); }
  int get_map_zoom() override { return _state->map_zoom(); }
  bool is_single_buffer() { return _state->is_single_buffer(); }
  void update_view() { broadcast_async<interface::DataUpdated>({}); }
  void auto_end() override { _capture_manager->auto_end(); }
  bool have_hardware_data();
  struct ds_device_base_info *get_device_list(int &out_count, int &actived_index);
  // 强制重新扫描所有驱动（热插拔检测场景）。get_device_list 默认复用缓存，
  // 避免在设备已 dev_open 后重复 sr_driver_scan 导致 LIBUSB_ERROR_ACCESS。
void refresh_device_list();
core::EventBus *get_event_bus() { return _event_bus.get(); }
template <typename EventType> void broadcast(const EventType &ev) { _event_bus->broadcast(ev); }
template <typename EventType> void broadcast_async(const EventType &ev) { _event_bus->broadcast_async(ev); }
  // Post an arbitrary callable to the main thread via postEvent (same
  // technique as broadcast_async). Used by Core-layer objects (e.g.
  // DecoderStack) that need to emit Qt signals from a worker thread —
  // emitting the signal on the main thread avoids QThreadData creation on
  // the worker thread (which crashes on thread exit, see eventbus.h:100-111).
  void event_bus_post(std::function<void()> fn) { _event_bus->dispatch_async(std::move(fn)); }
  bool have_new_realtime_refresh(bool keep) { return _capture_manager->have_new_realtime_refresh(keep); }
  std::shared_ptr<data::DecoderStack> get_decoder_trace(int index, data::SessionDocument *doc = nullptr);
  std::shared_ptr<data::SignalModel> get_signal_by_index(int index);
  bool have_view_data() override { return get_signal_snapshot()->have_data(); }
  bool is_copy_in_progress() const;
  data::SessionDocument *get_capture_owner_document() const;
void clear_capture_owner_document(data::SessionDocument *doc);
void on_load_config_end();
  void init_signals();
  bool is_doing_action() { return _capture_manager->is_action(); }
  void clear_view_data();
  void set_trace_name(std::shared_ptr<data::SignalModel> model, QString name);
  void set_decoder_row_label(int index, QString label);
  void set_decoder_pannel(IDecoderPannel *pannel) { _decoder_pannel = pannel; }
  void rebuild_decoder_pannel() {
      if (_decoder_pannel) _decoder_pannel->rebuild_layers();
      // Broadcast signals_changed so the View layer marks derived traces
      // dirty and syncs new DecoderStacks via sync_derived_traces().
      // Without this, MCP-added decoders (which bypass View::add_decoder)
      // are invisible in the GUI until a tab switch or repaint triggers sync.
      signals_changed();
  }
  void update_dso_data_scale() override;
  void remove_decode_task(std::shared_ptr<data::DecoderStack> stack);
  double get_logic_data_view_time() override;
  int64_t get_ring_sample_count();
  bool dso_data_is_out_off_range() { return _state->view_data()->get_dso()->data_is_out_off_range(); }
  void set_active_document(data::SessionDocument *doc);
  data::SessionDocument *get_active_document() override;
  void copy_data_to_document(data::SessionDocument *doc);
  void attach_data_to_signal(SessionData *data);
  const data::TriggerConfig& trigger_config() const override { return _state->trigger_config(); }
  void set_trigger_config(const data::TriggerConfig& cfg);
  // modernize-core-layer-radical phase 2: register/unregister removed.
  // Document ownership is now held by DocumentRegistry via take_document() /
  // release_document(). Use document_registry()->take_document(
  // make_unique<SessionDocument>(...)) to register, and
  // document_registry()->release_document(index) to release.
  core::DocumentRegistry *document_registry() { return _document_registry.get(); }
  void clear_all_documents_decoders();
  std::vector<std::shared_ptr<data::DecoderStack>> &decode_traces(data::SessionDocument *doc = nullptr) { return _state->decode_traces(doc); }
  void update_lang_text();
  bool have_decoded_result();
  void apply_samplerate();
  // 架构修复：thresholds/modes 用 channel_index 作 key，消除 View/Core 位置序号错位
  void set_glitch_filter(const std::map<int, uint32_t> &thresholds, const std::map<int, GlitchFilterMode> &filter_modes = {});
  void clear_glitch_filter();
  bool is_glitch_filter_active();
  // Per-channel glitch filter state (Task 9 / I4): public read accessors for
  // the current thresholds/modes so the View layer can snapshot prior state
  // before applying a new filter, then restore it via set_glitch_filter() on
  // undo_filter(). Returns references to the view-data maps; callers must
  // copy if they need a stable snapshot. Safe to call from the GUI thread.
  const std::map<int, uint32_t>& glitch_filter_thresholds() const { return _state->view_data()->_glitch_filter_thresholds; }
  const std::map<int, GlitchFilterMode>& glitch_filter_modes() const { return _state->view_data()->_glitch_filter_modes; }
  // 采集后自动重新应用滤波(保留上次阈值/模式)
  void set_glitch_filter_auto_apply(bool en) { _state->view_data()->_glitch_filter_auto_apply = en; }
  bool glitch_filter_auto_apply() const { return _state->view_data()->_glitch_filter_auto_apply; }
  // 显示波形轨道红色滤波提示叠加层
  void set_show_glitch_filter_overlay(bool en) { _state->view_data()->_show_glitch_filter_overlay = en; }
  bool show_glitch_filter_overlay() const { return _state->view_data()->_show_glitch_filter_overlay; }
  // 恢复持久化的滤波配置（从 .pxl/.pxc 加载），不触发实际滤波。
  // 实际滤波在采集完成后由 auto-apply 路径或用户手动应用时执行。
  void restore_glitch_filter_config(const std::map<int, uint32_t> &thresholds,
                                     const std::map<int, GlitchFilterMode> &modes) {
    _state->view_data()->_glitch_filter_thresholds = thresholds;
    _state->view_data()->_glitch_filter_modes = modes;
    // 标记为非 active —— 实际滤波未应用，但配置已恢复供 auto-apply 使用
    _state->view_data()->_glitch_filter_active = false;
  }
  // 新采集开始时清除滤波状态(不恢复数据,因为数据已被 clear)
  void clear_glitch_filter_state_for_capture();
  void set_signal_invert(const std::vector<bool> &channels);
  void clear_signal_invert();
  bool is_signal_invert_active();
  void restart_decoders();
  void start_all_decode_tasks() override;
  size_t get_disk_write_queue_depth();
  double get_disk_write_speed_mbps();
  bool is_disk_write_disk_full();

  // raw 版内存/磁盘缓冲指示（d39ee74a 的 raw 适配，用户拍板口径）：
  // 内存口径 = 已采集逻辑字节数（样本数/8），磁盘口径 = mmap 分配器当前文件大小。
  uint64_t get_logic_memory_bytes();
  uint64_t get_logic_disk_bytes();
  bool get_logic_disk_cache_active();
private:
  void set_cur_samplelimits(uint64_t samplelimits); void set_cur_snap_samplerate(uint64_t samplerate);
  void math_disable(); void sync_trigger_to_libsigrok(bool disable_trigger = false);
// Spec v2 Task 7: dispatch_to template removed (ISessionCallback abolished)
  void data_updated(); void set_receive_data_len(quint64 len); void receive_header();
  void cur_snap_samplerate_changed(); void frame_began(); void frame_ended();
  void update_capture(); void repeat_hold(int percent);
  void receive_trigger(quint64 trigger_pos); void show_wait_trigger();
  void signals_changed(); void session_error();
  void delay_prop_msg(QString strMsg);
  void clear_all_decode_task(int &runningDex); void clear_all_decode_task2();
  void add_decode_task(std::shared_ptr<data::DecoderStack> stack);
  void DeviceConfigChanged() override;
  // IDeviceAgentCallback — called from DeviceAgent's worker thread AFTER
  // sr_session_run() returns (libsigrok session fully stopped). Re-broadcasts
  // as the typed SessionStopped event via broadcast_async so listeners run on
  // the main thread. This is the upstream replacement for fork libsigrok's
  // DS_EV_COLLECT_TASK_END — the reliable "session really stopped" signal
  // that SR_DF_END cannot provide.
  void DeviceSessionStopped() override;
  // --- Core-internal state-machine event handlers ---
  // Called via EventBus::subscribe<T>() from the constructor.
  void on_device_options_updated();
  void on_trig_next_collect();
  void on_rev_end_packet();
  void on_copy_to_doc_done();
  void on_device_speed_not_match();
  void on_session_stopped_event();
  void on_decode_done_event();
  void on_end_collect_work_prev();
static sr_input_format *determine_input_file_format(const std::string &filename);
data::Snapshot *get_signal_snapshot(); void clear_signals();
std::shared_ptr<data::SignalModel> get_channel_by_index(int orgIndex);
void make_channels_view_index(int start_dex = -1);

// RAII subscriptions (auto-unsubscribe on destruction)
std::vector<core::Subscription> _event_subscriptions;

  // --- Shared mutable state (migrated to SessionStateContext) ---
  // All fields previously declared here (_sampling_mutex, _data_mutex,
  // _signal_models, _spectrum_stacks, _lissajous_model, _math_stack,
  // _session_time, _trig_time, _is_triged, _trigger_flag, _hw_replied,
  // _bClose, _is_saving, _trigger_ch, _error,
  // _error_pattern, _save_start, _save_end, _map_zoom, _is_working,
  // _device_status, _next_decoder_handle_id, _device_agent, _view_data,
  // _capture_data, _data_list, _trigger_config) now live on
  // SessionStateContext and are accessed via `_state->xxx()` accessors.
  // The 5 managers no longer hold a SigSession* — they hold a
  // SessionStateContext* and use the same accessors, eliminating the need
  // for friend declarations.
  std::unique_ptr<core::SessionStateContext> _state;

  // --- SigSession-retained ownership ---
  std::unique_ptr<core::EventBus> _event_bus;
  DeviceEventObject _device_event;
  IDecoderPannel *_decoder_pannel;
  std::unique_ptr<core::FilterProcessor> _filter_processor;
  std::unique_ptr<core::DecodeTaskManager> _decode_task_manager;
  std::unique_ptr<core::DataFeedParser> _data_feed_parser;
  std::unique_ptr<core::DocumentRegistry> _document_registry;
  std::unique_ptr<core::CaptureManager> _capture_manager;

  // Repeat + decoder gate. SessionStopped and DecodeDone may arrive in either order.
  bool _repeat_wait_decode = false;
  bool _repeat_session_stopped = false;
  bool _repeat_decode_done = false;
  bool _repeat_analog_trigger_active = false;
  bool _repeat_analog_trigger_match = false;
  bool _repeat_analog_trigger_display_hold = false;
  std::atomic<uint64_t> _repeat_analog_trigger_ui_generation{1};
  uint64_t _repeat_analog_trigger_sample = 0;
  data::DecoderAnalogTriggerConfig _repeat_analog_trigger_config;

  bool repeat_analog_display_trigger_enabled();
  void reset_repeat_analog_trigger_frame();
  void evaluate_repeat_analog_trigger();
  void set_repeat_analog_trigger_display_hold(bool hold);
  void continue_repeat_after_decode_if_ready();
  // Upstream libsigrok 0.6.0 context (single-lib architecture).
  // sr_context is created by sr_init() in init() and destroyed by sr_exit()
  // in uninit(). sr_session is now owned by DeviceAgent (created in
  // open_by_handle, destroyed in release) since the session lifecycle is
  // tied to the active device.
  struct sr_context *_sr_ctx = nullptr;

  // --- USB hotplug (libsigrok sr_listen_hotplug) ---
  // Hotplug callback runs on a libsigrok internal GThread; the static
  // trampoline forwards to the main thread via QMetaObject::invokeMethod
  // (Qt::QueuedConnection) so on_hotplug_event_() can safely touch Qt
  // objects and the EventBus. Reconnect watchdog gives a 500ms grace
  // period for the device to re-enumerate during capture before tearing
  // down the capture state. device_handle is the libusb_device* for both
  // ATTACH and DETACH (see hotplug.c); it is captured by value and used
  // for pointer-identity comparison (DETACH) and VID/PID matching (ATTACH
  // rebind). Comparing two pointer values is safe even after the
  // underlying libusb_device has been freed (no dereference is performed).
  static void hotplug_cb_(int event, void *user_data, void *device_handle);
  void on_hotplug_event_(int event, void *device_handle);
  std::unique_ptr<QTimer> reconnect_timer_;
  void start_reconnect_watchdog_();
  void on_reconnect_timeout_();
  // Returns true if the detached device (identified by device_handle, a
  // libusb_device*) is the currently-open device. nullptr device_handle is
  // treated conservatively as "current device gone" (safe fallback).
  bool is_current_device_gone_(void *device_handle);
  // Rebinds the active sdi to a freshly-scanned device matching the
  // current device's VID/PID. Called on ATTACH during the reconnect
  // watchdog window. device_handle is the ATTACHed libusb_device*
  // (reserved for future pointer-based matching; currently unused —
  // matching is by VID/PID). On mismatch or failure, falls back to
  // stop_capture() + set_default_device().
  void update_device_handle_(void *device_handle);

  // --- Async file import (Steps 5-7 of import_file) ---
  // The heavy file-reading + sr_input_send() loop runs on a background
  // std::async thread so the GUI main thread stays responsive for large
  // VCD/CSV/binary files. The future is joined (waited) in set_device(),
  // close_file(), and ~SigSession() before any device switch / teardown.
  std::future<void> _import_future;
  std::atomic<bool> _import_in_progress{false};
  // Joins _import_future if running. Called on the main thread before
  // releasing the current device (set_device, close_file, destructor).
  // Blocks until the background import thread finishes its current chunk.
  void wait_for_import_complete_();

  // --- DSO 测量缓存 (#3, 源自 c6cd59fd 非 RLE 部分) ---
  // MeasureCalculator::compute 在主线程 O(样本数) 遍历, hover 测量时频繁触发.
  // 缓存按 (data 指针, channel, ring_sample_count, view_rect_height) 键,
  // 数据变更 (running 时 ring 增长 / 文档切换) 自动失效, stopped hover 命中.
  mutable std::mutex _measure_cache_mutex;
  void *_measure_cache_data = nullptr;
  int _measure_cache_ch = -1;
  uint64_t _measure_cache_ring = 0;
  int _measure_cache_h = 0;
  bool _measure_cache_valid = false;
  std::vector<api::MeasurementValue> _measure_cache_val;
};

} // namespace pv
#endif // PXVIEW_PV_SIGSESSION_H
