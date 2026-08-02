#ifndef PXVIEW_CORE_SESSIONSTATECONTEXT_H
#define PXVIEW_CORE_SESSIONSTATECONTEXT_H

#include <QDateTime>
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "cursorregistry.h"
#include "../data/datasource.h"
#include "../data/mathstack.h"
#include "../data/sessiondata.h"
#include "../data/signalmodel.h"
#include "../data/triggerconfig.h"
#include "../deviceagent.h"
#include "../pxvdef.h"
#include <libsigrok/libsigrok.h>

namespace pv {

namespace data {
class LissajousModel;
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
 */
class SessionStateContext {
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
  void set_capture_manager(CaptureManager *m) { _capture_manager = m; }
  void set_decode_task_manager(DecodeTaskManager *m) {
    _decode_task_manager = m;
  }
  void set_data_feed_parser(DataFeedParser *m) { _data_feed_parser = m; }
  void set_document_registry(DocumentRegistry *m) { _document_registry = m; }
  void set_filter_processor(FilterProcessor *m) { _filter_processor = m; }

  CaptureManager *capture_manager() { return _capture_manager; }
  DecodeTaskManager *decode_task_manager() { return _decode_task_manager; }
  DataFeedParser *data_feed_parser() { return _data_feed_parser; }
  DocumentRegistry *document_registry() { return _document_registry; }
  FilterProcessor *filter_processor() { return _filter_processor; }

  // --- Mutex accessors (mutex is non-movable, exposed by reference) ---
  std::mutex &data_mutex() { return *_data_mutex; }
  std::mutex &sampling_mutex() { return *_sampling_mutex; }

  // --- Business objects ---
  std::vector<std::shared_ptr<data::SignalModel>> &signal_models() {
    return _signal_models;
  }
  std::vector<std::shared_ptr<data::SpectrumStack>> &spectrum_stacks() {
    return _spectrum_stacks;
  }
  data::LissajousModel *lissajous_model() { return _lissajous_model; }
  void set_lissajous_model(data::LissajousModel *m) { _lissajous_model = m; }
  const std::shared_ptr<data::MathStack> &math_stack() const {
    return _math_stack;
  }
  void set_math_stack(std::shared_ptr<data::MathStack> m) {
    _math_stack = std::move(m);
  }

  // --- Time ---
  QDateTime session_time() const { return _session_time; }
  void set_session_time(QDateTime t) { _session_time = t; }
  QDateTime trig_time() const { return _trig_time; }
  void set_trig_time(QDateTime t) { _trig_time = t; }

  // --- Bool state ---
  bool is_triged() const { return _is_triged; }
  void set_is_triged(bool v) { _is_triged = v; }
  bool trigger_flag() const { return _trigger_flag; }
  void set_trigger_flag(bool v) { _trigger_flag = v; }
  bool hw_replied() const { return _hw_replied; }
  void set_hw_replied(bool v) { _hw_replied = v; }
  bool bClose() const { return _bClose; }
  void set_bClose(bool v) { _bClose = v; }
  bool is_saving() const { return _is_saving; }
  void set_saving(bool v) { _is_saving = v; }

  // --- Numeric state ---
  uint8_t trigger_ch() const { return _trigger_ch; }
  void set_trigger_ch(uint8_t v) { _trigger_ch = v; }
  SESSION_ERROR_STATUS error() const { return _error; }
  void set_error(SESSION_ERROR_STATUS e) { _error = e; }
  uint64_t error_pattern() const { return _error_pattern; }
  void set_error_pattern(uint64_t v) { _error_pattern = v; }
  uint64_t save_start() const { return _save_start; }
  void set_save_start(uint64_t v) { _save_start = v; }
  uint64_t save_end() const { return _save_end; }
  void set_save_end(uint64_t v) { _save_end = v; }
  int map_zoom() const { return _map_zoom; }
  void set_map_zoom(int v) { _map_zoom = v; }

  // --- Atomic state ---
  bool is_working() const { return _is_working.load(); }
  void set_is_working(bool v) { _is_working.store(v); }
  int device_status() const { return _device_status.load(); }
  void set_device_status(int v) { _device_status.store(v); }

  // --- Device ---
  DeviceAgent &device_agent() { return _device_agent; }

  // --- Data buffers ---
  SessionData *view_data() { return _view_data; }
  void set_view_data(SessionData *d) { _view_data = d; }
  SessionData *capture_data() { return _capture_data; }
  void set_capture_data(SessionData *d) { _capture_data = d; }
  std::vector<SessionData *> &data_list() { return _data_list; }
  bool is_single_buffer() const { return _view_data == _capture_data; }

  // --- Trigger config ---
  const data::TriggerConfig &trigger_config() const { return _trigger_config; }
  void set_trigger_config(const data::TriggerConfig &c) { _trigger_config = c; }

  // --- Cursor registry (Task C2: cursor position state lives in Core so
  //     headless MCP clients can enumerate/mutate cursors without a View).
  //     View-layer view::Cursor objects are pure rendering objects that
  //     read/write their position through this registry via the DataSource
  //     interface (SigSession::get_cursors / add_cursor / remove_cursor /
  //     set_cursor_position). ---
  CursorRegistry &cursor_registry() { return _cursor_registry; }
  const CursorRegistry &cursor_registry() const { return _cursor_registry; }

  // --- EventBus dispatch helpers (migrated from SigSession private methods)---
  void data_updated();
  void set_receive_data_len(quint64 len);
  void receive_header();
  void cur_snap_samplerate_changed();
  void frame_began();
  void frame_ended();
  void update_capture();
  void repeat_hold(int percent);
  void receive_trigger(quint64 trigger_pos);
  void show_wait_trigger();
  void signals_changed();
  void session_error();
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
  void clear_all_decode_task2();
  void add_decode_task(std::shared_ptr<data::DecoderStack> stack);
  void attach_data_to_signal(SessionData *data);
  // Core→libsigrok 触发配置唯一同步点。
  // disable_trigger=true 时（instant 模式）清除 session 上的 sr_trigger，
  // 让所有 driver（demo/pxlogic/fx2lafw）都不等待触发，立即采集数据。
  // 这是统一入口，避免在每个 driver 内部单独判断 instant 标志。
  void sync_trigger_to_libsigrok(bool disable_trigger = false);
  void clear_glitch_filter_state_for_capture();
  uint16_t get_ch_num(int type);
  uint64_t cur_samplelimits();
  uint64_t cur_snap_samplerate();
  void set_cur_snap_samplerate(uint64_t samplerate);
  void set_cur_samplelimits(uint64_t samplelimits);

  // --- Decode-stack handle id generator (kept on state for centralized
  // access by both SigSession::add_decoder and DecodeTaskManager) ---
  uint64_t next_decoder_handle_id() { return _next_decoder_handle_id.fetch_add(1); }

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
  std::vector<std::shared_ptr<data::SpectrumStack>> _spectrum_stacks;
  data::LissajousModel *_lissajous_model = nullptr;
  std::shared_ptr<data::MathStack> _math_stack = nullptr;

  QDateTime _session_time, _trig_time;

  bool _is_triged = false, _trigger_flag = false, _hw_replied = false;
  bool _bClose = false, _is_saving = false;

  uint8_t _trigger_ch = 0;
  SESSION_ERROR_STATUS _error = No_err;
  uint64_t _error_pattern = 0, _save_start = 0, _save_end = 0;
  int _map_zoom = 0;

  std::atomic<bool> _is_working{false};
  std::atomic<int> _device_status{0}; // ST_INIT = 0
  std::atomic<uint64_t> _next_decoder_handle_id{1};

  DeviceAgent _device_agent;

  SessionData *_view_data = nullptr;
  SessionData *_capture_data = nullptr;
  std::vector<SessionData *> _data_list;

  data::TriggerConfig _trigger_config;

  // Task C2: cursor position state mirror. Owned by the state context so
  // both SigSession (Core) and the View layer (via DataSource) share one
  // source of truth. Position-indexed (see CursorRegistry docs).
  CursorRegistry _cursor_registry;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_SESSIONSTATECONTEXT_H
