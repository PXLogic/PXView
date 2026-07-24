#ifndef PXVIEW_CORE_CAPTUREMANAGER_H
#define PXVIEW_CORE_CAPTUREMANAGER_H

#include <QDateTime>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "../data/disk_cache_config.h"
#include "../data/sessiondata.h"
#include "../dstimer.h"
#include "../dsvdef.h" // DEVICE_COLLECT_MODE / DEVICE_STATUS_TYPE

namespace pv {

namespace data {
class SessionDocument;
} // namespace data

namespace core {

class EventBus;
class SessionStateContext;

/**
 * CaptureManager — owns the capture lifecycle (start/stop/exec/exit),
 * the DsTimer instances that drive data-feed/refresh/repeat cadence, and
 * the related flags/counters (_is_instant, _is_stream_mode, _is_action,
 * _clt_mode, _noData_cnt, _data_lock, _data_updated, _data_auto_lock,
 * _repeat_intvl, _repeat_hold_prg, _repeat_wait_prog_step, _work_time_id,
 * _capture_times, _confirm_store_time_id, _rt_refresh_time_id,
 * _rt_ck_refresh_time_id, _dso_packet_count, _disk_cache_config).
 * Extracted from SigSession (SubTask 10.6) as a mechanical refactoring:
 * no behavior change, just code movement.
 *
 * Holds an injected EventBus* (for typed event dispatch via
 * broadcast_async<T>/broadcast_sync<T>) and a SessionStateContext* (for
 * accessing shared session state: device_agent / capture_data / view_data /
 * signal_models /
 * document_registry / is_working / device_status / is_triged / trig_time /
 * etc.).
 */
class CaptureManager {
public:
  // Timer cadence constants (migrated from SigSession so this manager no
  // longer needs to include sigsession.h). SigSession re-exports them via
  // using-declarations for backward compatibility with View-layer callers.
  static constexpr int RefreshTime = 500;
  static constexpr int RepeatHoldDiv = 20;
  static constexpr int FeedInterval = 50;
  static constexpr int WaitShowTime = 500;

  CaptureManager(EventBus *bus, SessionStateContext *state);
  ~CaptureManager();

  // --- Capture lifecycle ---
  bool start_capture(bool instant, data::SessionDocument *owner = nullptr);
  bool stop_capture();
  bool exec_capture();
  void exit_capture();
  void capture_init();

  // --- action wrappers (set _is_action around the inner call) ---
  bool action_start_capture(bool instant,
                            data::SessionDocument *owner = nullptr);
  bool action_stop_capture();

  // --- Status queries ---
  // Synthesized from the Core trigger flag (is_triged, set by DataFeedParser
  // on first data packet) and the active capture buffer's sample count vs
  // cur_samplelimits(). Fork libsigrok's sr_status/ds_get_actived_device_status
  // are gone; upstream libsigrok does not expose per-sample progress, so this
  // derivation replaces them. Always returns true; triggered/progress convey
  // the state (used by the sidebar arc text and MCP get_capture_status).
  bool get_capture_status(bool &triggered, int &progress);
  int get_repeat_hold();
  bool is_first_store_confirm();
  bool is_realtime_refresh();
  bool have_new_realtime_refresh(bool keep);
  bool is_repeating();
  bool is_single_mode();
  bool is_repeat_mode();
  bool is_loop_mode();
  int get_collect_mode();
  void set_collect_mode(DEVICE_COLLECT_MODE m);

  inline bool is_instant() { return _is_instant; }
  inline void set_is_instant(bool v) { _is_instant = v; }
  inline bool is_stream_mode() { return _is_stream_mode; }
  inline void set_is_stream_mode(bool v) { _is_stream_mode = v; }
  inline bool is_action() { return _is_action; }
  inline double get_repeat_intvl() { return _repeat_intvl; }
  inline void set_repeat_intvl(double interval) { _repeat_intvl = interval; }
  inline void set_repeat_hold_prg(int v) { _repeat_hold_prg = v; }
  inline void set_repeat_wait_prog_step(int v) { _repeat_wait_prog_step = v; }
  inline int capture_times() const { return _capture_times; }
  // DsTimer Start/Stop are non-const, so expose typed wrapper methods
  // instead of leaking mutable refs to the timer sub-objects.
  inline void start_repeat_timer(int ms) { _repeat_timer.Start(ms); }
  inline void start_repeat_wait_prog_timer(int ms) {
    _repeat_wait_prog_timer.Start(ms);
  }
  inline void stop_trig_check_timer() { _trig_check_timer.Stop(); }

  inline void clear_store_confirm_flag() {
    _confirm_store_time_id = _work_time_id;
  }

  // --- Data lock state (migrated from SigSession) ---
  inline bool is_data_lock() { return _data_lock; }
  inline void data_lock() { _data_lock = true; }
  inline void data_unlock() { _data_lock = false; }
  void data_auto_lock(int lock);
  void data_auto_unlock();
  bool get_data_auto_lock();

  inline void set_data_updated(bool v) { _data_updated = v; }
  inline uint64_t dso_packet_count() const { return _dso_packet_count; }
  inline void inc_dso_packet_count() { ++_dso_packet_count; }

  // --- Decode result clearing (tightly coupled with capture start) ---
  void clear_decode_result();

  // --- DsTimer callback targets ---
  void feed_timeout();
  void nodata_timeout();
  void repeat_capture_wait_timeout();
  void repeat_wait_prog_timeout();
  void realtime_refresh_timeout();
  void trig_check_timeout();

  // --- DsTimer-driven data update check ---
  void check_update();

  // --- Re-initialize data buffers ---
  void refresh(int holdtime);

  // --- Auto-end hook (View-layer responsibility; kept as a no-op stub) ---
  void auto_end();

private:
  EventBus *_event_bus;
  // Shared session state (device_agent / capture_data / view_data /
  // signal_models / document_registry / is_working / device_status /
  // is_triged / trigger_flag / bClose / data_mutex / decode_traces() /
  // attach_data_to_signal() / sync_trigger_to_libsigrok() /
  // clear_all_decode_task2() / add_decode_task() / set_cur_snap_samplerate() /
  // set_cur_samplelimits() / set_session_time() / update_capture() /
  // repeat_hold() / clear_glitch_filter_state_for_capture() / get_ch_num() /
  // cur_samplelimits()) is accessed via SessionStateContext accessors.
  // modernize-core-layer-radical phase 1 broke the SigSession circular
  // dependency.
  SessionStateContext *_state;

  data::DiskCacheConfig _disk_cache_config;

  DsTimer _feed_timer;
  DsTimer _out_timer;
  DsTimer _repeat_timer;
  DsTimer _repeat_wait_prog_timer;
  DsTimer _refresh_rt_timer;
  DsTimer _trig_check_timer;

  int _noData_cnt;
  bool _data_lock;
  bool _data_updated;
  int _data_auto_lock;

  double _repeat_intvl; // The progress wait timer interval.
  int _repeat_hold_prg; // The time sleep progress
  int _repeat_wait_prog_step;
  bool _is_instant;
  int _work_time_id;
  int _capture_times;
  int _confirm_store_time_id;
  uint64_t _rt_refresh_time_id;
  uint64_t _rt_ck_refresh_time_id;
  DEVICE_COLLECT_MODE _clt_mode;
  bool _is_stream_mode;

  bool _is_action;
  uint64_t _dso_packet_count;

  // External access to the fields above is via public accessors only
  // (no friend declarations). SigSession / DataFeedParser call accessor
  // methods instead of touching private members directly.
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_CAPTUREMANAGER_H
