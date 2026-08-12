#ifndef PXVIEW_CORE_FILTERPROCESSOR_H
#define PXVIEW_CORE_FILTERPROCESSOR_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdint>

#include "pv/core/thread_pool.h"

// Spec v2 Task 4/5: GlitchFilterMode is now an enum class with fixed
// underlying type (int), so it can be forward-declared here without
// including the heavy logicsnapshot.h header.
enum class GlitchFilterMode : int;

#include "pv/core/isession_coordination.h"
#include "pv/core/isession_state.h"
#include "pv/core/isession_state.h"

namespace pv {

namespace core {

class EventBus;
class SessionStateContext;

/**
 * FilterProcessor — owns the glitch filter and signal invert background
 * threads and their running flags. Extracted from SigSession (SubTask 10.5)
 * as a mechanical refactoring: no behavior change, just code movement.
 *
 * Gap 1 fix: background threads now use ThreadPool instead of raw
 * std::thread. This eliminates the self-join detection logic and
 * manual join/detach patterns.
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

  /// Stop both background tasks. Called from SigSession::Close().
  void stop();

private:
  void glitch_filter_task(const std::map<int, uint32_t> thresholds,
                          const std::map<int, GlitchFilterMode> filter_modes);
  void signal_invert_task(const std::vector<bool> channels);

  EventBus *_event_bus;
  ISessionState *_state;
  ISessionCoordination *_coord;

  // Gap 1: ThreadPool replaces unique_ptr<std::thread>.
  // ThreadPool manages thread lifetime — no manual join, no self-join risk.
  ThreadPool _filter_pool;

  std::atomic<bool> _glitch_filter_running;
  // S1/H2 fix: mutex protects the launch path (check _running + submit task)
  // so concurrent callers cannot both see _running==false and create
  // duplicate tasks.
  std::mutex _glitch_launch_mutex;
  // 架构修复：滤波运行中排队最近一次请求，不再静默丢弃
  std::atomic<bool> _has_pending_glitch{false};
  std::map<int, uint32_t> _pending_glitch_thresholds;
  std::map<int, GlitchFilterMode> _pending_glitch_modes;
  std::mutex _pending_mutex;
  std::atomic<bool> _signal_invert_running;
  // H2 fix: same TOCTOU protection for signal invert launch path
  std::mutex _signal_invert_launch_mutex;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_FILTERPROCESSOR_H
