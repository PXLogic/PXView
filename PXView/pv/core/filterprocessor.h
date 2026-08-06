#ifndef PXVIEW_CORE_FILTERPROCESSOR_H
#define PXVIEW_CORE_FILTERPROCESSOR_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>

// Spec v2 Task 4/5: GlitchFilterMode is now an enum class with fixed
// underlying type (int), so it can be forward-declared here without
// including the heavy logicsnapshot.h header.
enum class GlitchFilterMode : int;

#include "isession_coordination.h"
#include "isession_state.h"
#include "isession_state.h"

namespace pv {

namespace core {

class EventBus;
class SessionStateContext;

/**
 * FilterProcessor — owns the glitch filter and signal invert background
 * threads and their running flags. Extracted from SigSession (SubTask 10.5)
 * as a mechanical refactoring: no behavior change, just code movement.
 *
 * The processor holds an injected EventBus* (for typed event dispatch via
 * broadcast_async<T>/broadcast_sync<T>) and a SessionStateContext* (for
 * accessing view_data / device_agent / data_updated()).
 * modernize-core-layer-radical phase 1 replaced the previous SigSession* +
 * friend-declaration coupling.
 */
class FilterProcessor {
public:
  FilterProcessor(EventBus *bus, ISessionState *state, ISessionCoordination *coord);
  ~FilterProcessor();

  // 架构修复：thresholds/modes 用 channel_index 作 key，消除 View/Core 位置序号错位
  void set_glitch_filter(const std::map<int, uint32_t> &thresholds,
                         const std::map<int, GlitchFilterMode> &filter_modes);
  void clear_glitch_filter();
  bool is_glitch_filter_active();

  void set_signal_invert(const std::vector<bool> &channels);
  void clear_signal_invert();
  bool is_signal_invert_active();

  /// Stop both background threads. Called from SigSession::Close().
  void stop();

private:
  void glitch_filter_task(const std::map<int, uint32_t> thresholds,
                          const std::map<int, GlitchFilterMode> filter_modes);
  void signal_invert_task(const std::vector<bool> channels);

  EventBus *_event_bus;
  ISessionState *_state;
  ISessionCoordination *_coord;

  // modernize-core-layer-final Task 5: RAII-managed background threads.
  // unique_ptr replaces raw std::thread* + manual new/delete. The destructor
  // path joins (if joinable) and resets the pointer automatically — no
  // `delete` calls remain in the .cpp.
  std::unique_ptr<std::thread> _glitch_filter_thread;
  std::atomic<bool> _glitch_filter_running;
  // 架构修复：滤波运行中排队最近一次请求，不再静默丢弃
  // Track A4: pending data protected by _pending_mutex; _has_pending_glitch
  // uses std::atomic<bool> as a fast flag, but map data still needs mutex.
  std::atomic<bool> _has_pending_glitch{false};
  std::map<int, uint32_t> _pending_glitch_thresholds;
  std::map<int, GlitchFilterMode> _pending_glitch_modes;
  std::mutex _pending_mutex;
  std::unique_ptr<std::thread> _signal_invert_thread;
  std::atomic<bool> _signal_invert_running;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_FILTERPROCESSOR_H
