#ifndef PXVIEW_CORE_CAPTUREMANAGER_H
#define PXVIEW_CORE_CAPTUREMANAGER_H

#include <QDateTime>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "pv/data/cache/disk_cache_config.h"
#include "pv/data/document/sessiondata.h"
#include "pv/base/dstimer.h"
#include "pv/base/pxvdef.h" // DEVICE_COLLECT_MODE / DEVICE_STATUS_TYPE
#include "pv/core/isession_coordination.h"
#include "pv/core/isession_state.h"
#include "pv/core/isession_state.h"

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

  CaptureManager(EventBus *bus, ISessionState *state, ISessionCoordination *coord);
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
  int get_repeat_hold() const;
  bool is_first_store_confirm();
  bool is_realtime_refresh() const;
  bool have_new_realtime_refresh(bool keep);
  bool is_repeating() const;
  bool is_single_mode() const;
  bool is_repeat_mode() const;
  bool is_loop_mode() const;
  int get_collect_mode() const;
  void set_collect_mode(DEVICE_COLLECT_MODE m);

  inline bool is_instant() const { return _is_instant.load(); }
  inline void set_is_instant(bool v) { _is_instant.store(v); }
  inline bool is_stream_mode() const { return _is_stream_mode.load(); }
  inline void set_is_stream_mode(bool v) { _is_stream_mode.store(v); }
  inline bool is_action() const { return _is_action.load(); }
  inline double get_repeat_intvl() const { return _repeat_intvl.load(); }
  inline void set_repeat_intvl(double interval) { _repeat_intvl.store(interval); }
  inline void set_repeat_hold_prg(int v) { _repeat_hold_prg.store(v); }
  inline void set_repeat_wait_prog_step(int v) { _repeat_wait_prog_step.store(v); }
  inline int capture_times() const { return _capture_times.load(); }
  // DsTimer Start/Stop are non-const, so expose typed wrapper methods
  // instead of leaking mutable refs to the timer sub-objects.
  inline void start_repeat_timer(int ms) { _repeat_timer.Start(ms); }
  inline void start_repeat_wait_prog_timer(int ms) {
    _repeat_wait_prog_timer.Start(ms);
  }
  inline void stop_trig_check_timer() { _trig_check_timer.Stop(); }

  inline void clear_store_confirm_flag() {
    _confirm_store_time_id.store(_work_time_id.load());
  }

  // --- Data lock state (migrated from SigSession) ---
  inline bool is_data_lock() const { return _data_lock.load(); }
  inline void data_lock() { _data_lock.store(true); }
  inline void data_unlock() { _data_lock.store(false); }
  void data_auto_lock(int lock);
  void data_auto_unlock();
  bool get_data_auto_lock() const;

  inline void set_data_updated(bool v) { _data_updated.store(v); }
  inline uint64_t dso_packet_count() const { return _dso_packet_count.load(); }
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
  // Shared session state accessed via SessionStateContext accessors.
  ISessionState *_state;
  // Cross-manager coordination accessed via interface (Spec v3 Task 5)
  ISessionCoordination *_coord;

  data::DiskCacheConfig _disk_cache_config;

  DsTimer _feed_timer;
  DsTimer _out_timer;
  DsTimer _repeat_timer;
  DsTimer _repeat_wait_prog_timer;
  DsTimer _refresh_rt_timer;
  DsTimer _trig_check_timer;

  std::atomic<int> _noData_cnt;
  std::atomic<bool> _data_lock;
  std::atomic<bool> _data_updated;
  std::atomic<int> _data_auto_lock;

  std::atomic<double> _repeat_intvl; // The progress wait timer interval.
  std::atomic<int> _repeat_hold_prg; // The time sleep progress
  std::atomic<int> _repeat_wait_prog_step;
  std::atomic<bool> _is_instant;
  std::atomic<int> _work_time_id;
  std::atomic<int> _capture_times;
  std::atomic<int> _confirm_store_time_id;
  std::atomic<uint64_t> _rt_refresh_time_id;
  std::atomic<uint64_t> _rt_ck_refresh_time_id;
  DEVICE_COLLECT_MODE _clt_mode; // main-thread-only (set in action_start_capture)
  std::atomic<bool> _is_stream_mode;

  std::atomic<bool> _is_action;
  std::atomic<uint64_t> _dso_packet_count;

  // External access to the fields above is via public accessors only
  // (no friend declarations). SigSession / DataFeedParser call accessor
  // methods instead of touching private members directly.
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_CAPTUREMANAGER_H
