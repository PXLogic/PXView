#ifndef PXVIEW_CORE_SESSIONSTATECONTEXT_H
#define PXVIEW_CORE_SESSIONSTATECONTEXT_H

#include <QDateTime>
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "pv/core/cursorregistry.h"
#include "pv/core/shared_state.h"
#include "pv/data/datasource.h"
#include "pv/data/document/sessiondata.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/triggerconfig.h"
#include "pv/session/deviceagent.h"
#include "pv/base/pxvdef.h"
#include "pv/core/isession_coordination.h"
#include "pv/core/isession_state.h"

namespace pv {

namespace data {
class LissajousModel;
class MathStack;
class SessionDocument;
class SpectrumStack;
class DecoderStack;
} // namespace data

namespace core {

class EventBus;
class CaptureManager;
class DecodeTaskManager;
class DataFeedParser;
class DocumentRegistry;
class FilterProcessor;

/**
 * SessionStateContext — shared session state holder for the Core layer.
 *
 * Introduced by modernize-core-layer-radical phase 1 to break the circular
 * dependency between SigSession and its 5 managers (CaptureManager /
 * DocumentRegistry / DecodeTaskManager / DataFeedParser / FilterProcessor).
 * Previously each manager held a `SigSession*` and reached into SigSession's
 * private fields via friend declarations (263 direct `_session->_xxx` access
 * sites). Now each manager holds a `SessionStateContext*` and uses accessor
 * methods, so SigSession no longer needs to friend the managers.
 *
 * Owns the shared mutable state (mutexes, signal models, device agent, view/
 * capture data, atomic flags, trigger config, etc.) and hosts the EventBus
 * dispatch helpers (data_updated / frame_began / etc.) that were previously
 * private methods on SigSession. Manager pointers are injected
 * post-construction so cross-manager helpers (decode_traces / attach_data_to_
 * signal / sync_trigger_to_libsigrok / clear_all_decode_task2) can be hosted
 * here too.
 *
 * Lifetime: owned by SigSession via unique_ptr. Raw manager back-pointers are
 * set by SigSession::init() after the managers are constructed, and remain
 * valid for the lifetime of the SessionStateContext (managers are destroyed
 * before _state in the SigSession destructor).
 *
 * Spec v2 Task 10: Implements ISessionCoordination to expose cross-manager
 * coordination methods via an interface, breaking the circular dependency
 * between SessionStateContext and its 5 managers.
 */
class SessionStateContext : public ISessionState {
public:
  /// Error status enum (migrated from SigSession::SESSION_ERROR_STATUS so
  /// SessionStateContext does not need to depend on SigSession). SigSession
  /// re-exports the values via using-declarations for backward compatibility.
  enum SESSION_ERROR_STATUS {
    No_err,
    Hw_err,
    Malloc_err,
    Test_timeout_err,
    Pkt_data_err,
    Data_overflow
  };

  SessionStateContext();
  ~SessionStateContext();

  SessionStateContext(const SessionStateContext &) = delete;
  SessionStateContext &operator=(const SessionStateContext &) = delete;

  // --- EventBus (injected by SigSession before manager construction) ---
  void set_event_bus(EventBus *bus) { _event_bus = bus; }
  EventBus *event_bus() { return _event_bus; }

  // --- Manager back-pointers (injected by SigSession after construction) ---
  // Manager setters (inject by SigSession after construction)
  void set_capture_manager(CaptureManager *m) { _capture_manager = m; }
  void set_decode_task_manager(DecodeTaskManager *m) {
    _decode_task_manager = m;
  }
  void set_data_feed_parser(DataFeedParser *m) { _data_feed_parser = m; }
  void set_document_registry(DocumentRegistry *m) { _document_registry = m; }
  void set_filter_processor(FilterProcessor *m) { _filter_processor = m; }

  // capture_manager() / decode_task_manager() are ISessionCoordination overrides
  // (see Spec v3 Task 5 section below)
  DataFeedParser *data_feed_parser() { return _data_feed_parser; }
  DocumentRegistry *document_registry() { return _document_registry; }
  FilterProcessor *filter_processor() { return _filter_processor; }

  // --- Mutex accessors (mutex is non-movable, exposed by reference) ---
  std::mutex &data_mutex() { return *_data_mutex; }
  std::mutex &sampling_mutex() { return *_sampling_mutex; }

  // --- Business objects ---
  // signal_models() returns the live vector reference. Callers that access
  // it from non-UI threads (decode thread, save/export thread) MUST hold a
  // shared_lock on signal_models_mutex() for the duration of their access.
  // Writers (init_signals, reload) MUST hold a unique_lock.
  std::vector<std::shared_ptr<data::SignalModel>> &signal_models() {
    return _signal_models;
  }
  // TS-2 fix: thread-safe snapshot — copies the vector under a shared_lock
  // so callers don't need to manually acquire the mutex. Safe from any
  // thread. Prefer this over signal_models() when you only need to iterate.
  std::vector<std::shared_ptr<data::SignalModel>> signal_models_snapshot() {
    std::shared_lock<std::shared_mutex> lk(_signal_models_mutex);
    return _signal_models;
  }
  std::shared_mutex &signal_models_mutex() { return _signal_models_mutex; }
  std::vector<std::shared_ptr<data::SpectrumStack>> &spectrum_stacks() {
    return _spectrum_stacks;
  }
  // Track B2: LissajousModel owned via unique_ptr
  data::LissajousModel *lissajous_model() const { return _lissajous_model.get(); }
  void set_lissajous_model(std::unique_ptr<data::LissajousModel> m);
  const std::shared_ptr<data::MathStack> &math_stack() const {
    return _math_stack;
  }
  void set_math_stack(std::shared_ptr<data::MathStack> m) {
    _math_stack = std::move(m);
  }

  // --- Time (getters only; setters are ISessionCoordination overrides) ---
  QDateTime session_time() const { return _session_time; }
  QDateTime trig_time() const { return _trig_time; }

  // --- Bool state (getters only; setters are ISessionCoordination overrides) ---
  // Track A5: cross-thread bool flags use std::atomic<bool>
  bool is_triged() const { return _is_triged.load(); }
  bool trigger_flag() const { return _trigger_flag.load(); }
  bool hw_replied() const { return _hw_replied.load(); }
  void set_bClose(bool v) { _bClose.store(v); }
  bool is_saving() const { return _is_saving.load(); }
  void set_saving(bool v) { _is_saving.store(v); }

  // --- Numeric state ---
  int trigger_ch() const { return _trigger_ch; }
  void set_trigger_ch(int v) override { _trigger_ch = static_cast<uint8_t>(v); }
  SESSION_ERROR_STATUS error() const { return _error; }
  void set_error(int e) override { _error = static_cast<SESSION_ERROR_STATUS>(e); }
  uint64_t error_pattern() const { return _error_pattern; }
  void set_error_pattern(uint64_t v) { _error_pattern = v; }
  uint64_t save_start() const { return _save_start; }
  void set_save_start(uint64_t v) { _save_start = v; }
  uint64_t save_end() const { return _save_end; }
  void set_save_end(uint64_t v) { _save_end = v; }
  int map_zoom() const { return _map_zoom; }
  void set_map_zoom(int v) { _map_zoom = v; }

  // --- Atomic state (is_working getter only; setter is ISessionCoordination override) ---
  bool is_working() const { return _is_working.load(); }
  int device_status() const { return _device_status.load(); }
  void set_device_status(int v) { _device_status.store(v); }

  // --- Device ---
  DeviceAgent &device_agent() { return _device_agent; }

  // --- Data buffers ---
  // Thread-safety P1: _view_data / _capture_data are now
  // std::atomic<SessionData*> for safe cross-thread reads.
  // Writers (main thread) use store(); readers (decode thread,
  // data feed thread) use load().
  SessionData *view_data() { return _view_data.load(std::memory_order_acquire); }
  void set_view_data(SessionData *d) { _view_data.store(d, std::memory_order_release); }
  SessionData *capture_data() { return _capture_data.load(std::memory_order_acquire); }
  // Track B1: _data_list owns SessionData via unique_ptr
  std::vector<std::unique_ptr<SessionData>> &data_list() { return _data_list; }
  bool is_single_buffer() const { return _view_data.load() == _capture_data.load(); }

  // --- Trigger config ---
  // Thread-safety P2: set_trigger_config() is protected by
  // _trigger_config_mutex. trigger_config() returns const ref for
  // interface compatibility (ISessionState / ISignalSource) and is
  // main-thread-only — all mutation paths now dispatch to main thread
  // via run_void_on_main_thread (P0 fix).
  const data::TriggerConfig &trigger_config() const { return _trigger_config; }
  void set_trigger_config(const data::TriggerConfig &c) {
      std::lock_guard<std::mutex> lk(_trigger_config_mutex);
      _trigger_config = c;
  }

  // --- Cursor registry (Task C2: cursor position state lives in Core so
  //     headless MCP clients can enumerate/mutate cursors without a View).
  //     View-layer view::Cursor objects are pure rendering objects that
  //     read/write their position through this registry via the DataSource
  //     interface (SigSession::get_cursors / add_cursor / remove_cursor /
  //     set_cursor_position). ---
  CursorRegistry &cursor_registry() { return _cursor_registry; }
  const CursorRegistry &cursor_registry() const { return _cursor_registry; }

  // --- EventBus dispatch helpers (migrated from SigSession private methods)---
  // Spec v3 Task 5: notification methods are now ISessionCoordination overrides
  void data_updated() override;
  void set_receive_data_len(uint64_t len) override;
  void receive_header() override;
  void cur_snap_samplerate_changed();
  void frame_began() override;
  void frame_ended();
  void update_capture() override;
  void repeat_hold(int percent);
  void receive_trigger(uint64_t trigger_pos) override;
  void show_wait_trigger();
  void signals_changed() override;
  void session_error() override;
  void delay_prop_msg(QString strMsg);

  // --- Cross-manager helpers (migrated from SigSession) ---
  std::vector<std::shared_ptr<data::DecoderStack>> &
  decode_traces(data::SessionDocument *doc = nullptr);
  std::vector<std::shared_ptr<data::DecoderStack>> &
  get_decoder_stacks(data::SessionDocument *doc = nullptr);
  std::shared_ptr<data::DecoderStack>
  get_decoder_trace(int index, data::SessionDocument *doc = nullptr);
  int get_trace_index_by_key_handel(void *handel,
                                    data::SessionDocument *doc = nullptr);
  // --- State mutation overrides (Spec v3 Task 5) ---
  void set_trigger_flag(bool v) override { _trigger_flag.store(v); }
  void set_hw_replied(bool v) override { _hw_replied.store(v); }
  void set_capture_data(SessionData *d) override { _capture_data = d; }
  void set_session_time(QDateTime t) override { _session_time = t; }
  void set_is_working(bool v) override { _is_working.store(v); }
  void set_is_triged(bool v) override { _is_triged.store(v); }
  void set_trig_time(QDateTime t) override { _trig_time = t; }
  bool bClose() const override { return _bClose.load(); }

  // --- ISessionCoordination overrides (Spec v2 Task 10) ---
  void clear_all_decode_task2() override;
  void add_decode_task(std::shared_ptr<data::DecoderStack> stack) override;
  void attach_data_to_signal(SessionData *data) override;
  // Core→libsigrok 触发配置唯一同步点。
  // disable_trigger=true 时（instant 模式）清除 session 上的 sr_trigger，
  // 让所有 driver（demo/pxlogic/fx2lafw）都不等待触发，立即采集数据。
  // 这是统一入口，避免在每个 driver 内部单独判断 instant 标志。
  void sync_trigger_to_libsigrok(bool disable_trigger = false) override;
  void clear_glitch_filter_state_for_capture() override;
  uint16_t get_ch_num(int type) override;
  uint64_t cur_samplelimits() override;
  uint64_t cur_snap_samplerate() override;
  void set_cur_snap_samplerate(uint64_t samplerate) override;
  void set_cur_samplelimits(uint64_t samplelimits) override;

  // --- Plan B Phase 4: atomic state snapshot ---
  // Returns a consistent snapshot of the capture-related state. All fields
  // are read from atomics, so the snapshot is thread-safe.
  // Modeled after Logic2's DigitalStore::GetState() pattern (Pull).
  struct CaptureStateSnapshot {
    bool is_working;
    int device_status;      // ST_INIT / ST_RUNNING / ST_STOPPED
    bool is_copy_in_progress;
  };
  CaptureStateSnapshot get_capture_state_snapshot() const;

  // --- Decode-stack handle id generator (kept on state for centralized
  // access by both SigSession::add_decoder and DecodeTaskManager) ---
  uint64_t next_decoder_handle_id() { return _next_decoder_handle_id.fetch_add(1); }

  // --- Phase 3: capture-complete sync wait (via SharedState, bypasses Qt event queue) ---
  // Called by the worker thread (DeviceSessionStopped) when sr_session_run()
  // returns. Sets _is_working=false and signals the SharedState so that
  // wait_for_capture_complete() (called on the main thread by the API /
  // RPC layer) is woken immediately, without depending on the Qt event queue.
  //
  // Mirrors Logic2's SharedState::SetResult() pattern
  // (task_executor.h:191-196).
  void notify_capture_complete() {
    _is_working.store(false, std::memory_order_release);
    _capture_complete_state.set_result();
  }
  // Called by the main thread (SessionService::wait_capture_complete) to
  // block until _is_working becomes false or timeout. Does NOT use
  // QEventLoop::exec() — the wakeup comes directly from the worker thread's
  // notify_capture_complete() via SharedState.
  //
  // Mirrors Logic2's SharedState::WaitOnState() pattern
  // (task_executor.h:209-224).
  bool wait_for_capture_complete(uint64_t timeout_ms) {
    // Fast path: already complete
    if (!_is_working.load(std::memory_order_acquire))
      return true;
    return _capture_complete_state.wait(timeout_ms);
  }

  // --- Phase 3: decode-complete sync wait (via SharedState) ---
  // Called by the decode thread (DecodeTaskManager) when all decoders
  // finish. Signals the SharedState so that wait_for_decode_complete()
  // (called on the main thread by the API / RPC layer) is woken.
  void notify_decode_complete() {
    _decode_complete_state.set_result();
  }
  // Called by the main thread to block until decode finishes or timeout.
  bool wait_for_decode_complete(uint64_t timeout_ms) {
    return _decode_complete_state.wait(timeout_ms);
  }
  // Reset decode state before starting a new decode cycle.
  void reset_decode_complete() {
    _decode_complete_state.reset();
  }

  // --- Phase 3: reset capture state before starting a new capture ---
  void reset_capture_complete() {
    _capture_complete_state.reset();
  }

private:
  EventBus *_event_bus = nullptr;
  CaptureManager *_capture_manager = nullptr;
  DecodeTaskManager *_decode_task_manager = nullptr;
  DataFeedParser *_data_feed_parser = nullptr;
  DocumentRegistry *_document_registry = nullptr;
  FilterProcessor *_filter_processor = nullptr;

  // mutexes wrapped in unique_ptr (mutex is non-movable)
  std::unique_ptr<std::mutex> _sampling_mutex;
  std::unique_ptr<std::mutex> _data_mutex;

  std::vector<std::shared_ptr<data::SignalModel>> _signal_models;
  std::shared_mutex _signal_models_mutex;
  std::vector<std::shared_ptr<data::SpectrumStack>> _spectrum_stacks;
  // Track B2: LissajousModel owned via unique_ptr
  std::unique_ptr<data::LissajousModel> _lissajous_model;
  std::shared_ptr<data::MathStack> _math_stack = nullptr;

  QDateTime _session_time, _trig_time;

  // Track A5: cross-thread bool flags use std::atomic<bool>
  std::atomic<bool> _is_triged{false};
  std::atomic<bool> _trigger_flag{false};
  std::atomic<bool> _hw_replied{false};
  std::atomic<bool> _bClose{false};
  std::atomic<bool> _is_saving{false};

  uint8_t _trigger_ch = 0;
  SESSION_ERROR_STATUS _error = No_err;
  uint64_t _error_pattern = 0, _save_start = 0, _save_end = 0;
  int _map_zoom = 0;

  std::atomic<bool> _is_working{false};
  std::atomic<int> _device_status{0}; // ST_INIT = 0
  std::atomic<uint64_t> _next_decoder_handle_id{1};

  DeviceAgent _device_agent;

  std::atomic<SessionData *> _view_data{nullptr};
  std::atomic<SessionData *> _capture_data{nullptr};
  // Track B1: data buffers owned via unique_ptr
  std::vector<std::unique_ptr<SessionData>> _data_list;

  data::TriggerConfig _trigger_config;
  mutable std::mutex _trigger_config_mutex;

  // Task C2: cursor position state mirror. Owned by the state context so
  // both SigSession (Core) and the View layer (via DataSource) share one
  // source of truth. Position-indexed (see CursorRegistry docs).
  CursorRegistry _cursor_registry;

  // --- Phase 3: SharedState instances for sync waits ---
  // Replaces Phase 1's inline mutex + cv with the reusable SharedState
  // primitive (pv/core/shared_state.h), modeled after Logic2's
  // Saleae::Tasks::Detail::SharedState.
  //
  // _capture_complete_state: signaled by notify_capture_complete() on the
  //   worker thread, waited on by wait_for_capture_complete() on the main
  //   thread. Predicate: _is_working == false.
  //
  // _decode_complete_state: signaled by notify_decode_complete() on the
  //   decode thread, waited on by wait_for_decode_complete() on the main
  //   thread.
  SharedState _capture_complete_state;
  SharedState _decode_complete_state;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_SESSIONSTATECONTEXT_H
