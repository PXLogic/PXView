#include "pv/core/filterprocessor.h"

#include "pv/core/eventbus.h"
#include "pv/core/sessionstatecontext.h"
#include "pv/session/sigsession.h"  // SessionData full definition
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/base/log.h"

#include <libsigrok/libsigrok.h>

namespace pv {
namespace core {

FilterProcessor::FilterProcessor(EventBus *bus, ISessionState *state, ISessionCoordination *coord)
    : _event_bus(bus), _state(state), _coord(coord),
      _glitch_filter_running(false),
      _signal_invert_running(false) {}

FilterProcessor::~FilterProcessor() { stop(); }

void FilterProcessor::stop() {
  // A3 fix: Stop glitch filter and signal invert background threads before
  // tearing down data. Set running flags false first so the task functions
  // know no new work should be accepted, then join the thread if still
  // joinable. Without this, a joinable std::thread would std::terminate on
  // destruction.
  //
  // modernize-core-layer-final Task 5: threads are now held by unique_ptr.
  // No manual delete — reset() releases the thread object after join().
  //
  // M-2 fix: acquire the launch mutexes so that if a task thread is in the
  // middle of a recursive set_glitch_filter()/set_signal_invert() call,
  // we wait for it to finish launching (or queuing the pending request)
  // before we set _running=false and join. This closes the TOCTOU window
  // where stop() sets _running=false concurrently with the task thread
  // setting it back to true.
  {
    std::lock_guard<std::mutex> lk(_glitch_launch_mutex);
    _glitch_filter_running = false;
  }
  if (_glitch_filter_thread) {
    if (_glitch_filter_thread->joinable())
      _glitch_filter_thread->join();
    _glitch_filter_thread.reset();
  }
  {
    std::lock_guard<std::mutex> lk(_signal_invert_launch_mutex);
    _signal_invert_running = false;
  }
  if (_signal_invert_thread) {
    if (_signal_invert_thread->joinable())
      _signal_invert_thread->join();
    _signal_invert_thread.reset();
  }
}

void FilterProcessor::set_glitch_filter(
    const std::map<int, uint32_t> &thresholds,
    const std::map<int, GlitchFilterMode> &filter_modes) {
  // S1/H2 fix: lock the launch mutex for the entire check-and-create path.
  // This prevents two callers from both seeing _running==false and creating
  // duplicate threads. It also prevents the self-join deadlock: when the
  // task thread recursively calls set_glitch_filter() at the end of
  // glitch_filter_task(), it holds this mutex, and we detect that
  // _glitch_filter_thread is the calling thread via get_id() comparison.
  std::lock_guard<std::mutex> launch_lk(_glitch_launch_mutex);

  if (_glitch_filter_running) {
    // 架构修复：不再静默丢弃，排队最近一次请求，滤波完成后自动执行
    // Track A4: protect pending data with _pending_mutex
    std::lock_guard<std::mutex> lk(_pending_mutex);
    _pending_glitch_thresholds = thresholds;
    _pending_glitch_modes = filter_modes;
    _has_pending_glitch.store(true);
    return;
  }

  if (_state->view_data()->get_logic()->empty())
    return;

  bool has_filter = false;
  for (auto &kv : thresholds) {
    if (kv.second > 0) {
      has_filter = true;
      break;
    }
  }
  if (!has_filter)
    return;

  _glitch_filter_running = true;
  _event_bus->broadcast_async<interface::GlitchFilterStarted>({});

  // S1 fix: if the thread pointer is the calling thread itself (recursive
  // call from glitch_filter_task), reset it WITHOUT joining — joining
  // oneself is undefined behavior (resource_deadlock_would_occur).
  if (_glitch_filter_thread) {
    if (_glitch_filter_thread->get_id() == std::this_thread::get_id()) {
      // This is a recursive call from the task thread itself. The thread
      // object is about to go out of scope when we reassign, but it's
      // joinable. We must detach (not join) to avoid self-join UB.
      _glitch_filter_thread->detach();
      _glitch_filter_thread.reset();
    } else {
      if (_glitch_filter_thread->joinable())
        _glitch_filter_thread->join();
      _glitch_filter_thread.reset();
    }
  }

  _glitch_filter_thread = std::make_unique<std::thread>(
      &FilterProcessor::glitch_filter_task, this, thresholds, filter_modes);
}

void FilterProcessor::glitch_filter_task(
    const std::map<int, uint32_t> thresholds,
    const std::map<int, GlitchFilterMode> filter_modes) {
  if (!_state->view_data()->_logic_backup) {
    // Track B3: use make_unique instead of raw new
    _state->view_data()->_logic_backup = std::make_unique<data::LogicSnapshot>();
    _state->view_data()->_logic_backup->copy_from(
        *(_state->view_data()->get_logic()));
    if (_state->view_data()->_logic_backup->memory_failed()) {
      _state->view_data()->_logic_backup.reset();
      _glitch_filter_running = false;
      _event_bus->broadcast_async<interface::GlitchFilterCompleted>({});
      return;
    }
  } else {
    _state->view_data()->get_logic()->copy_from(
        *_state->view_data()->_logic_backup);
  }

  // 重新滤波前清空持久化区间（apply_glitch_filter 会重新累积，避免残留）
  if (_state->view_data()->get_logic()) {
    _state->view_data()->get_logic()->clear_filtered_ranges();
  }

  // If signal invert is active, apply invert before glitch filter
  if (_state->view_data()->_signal_invert_active) {
    int ch_idx = 0;
    for (const GSList *l = _state->device_agent().get_channels(); l;
         l = l->next) {
      sr_channel *const probe = (sr_channel *)l->data;
      if (probe->type != SR_CHANNEL_LOGIC)
        continue;
      if (ch_idx < (int)_state->view_data()->_signal_invert_channels.size() &&
          _state->view_data()->_signal_invert_channels[ch_idx]) {
        _state->view_data()->get_logic()->invert_channel(probe->index);
      }
      ch_idx++;
    }
  }

  _state->view_data()->get_logic()->apply_glitch_filter_all(
      thresholds,
      [this](int progress) {
        _event_bus->broadcast_async<interface::GlitchFilterProgress>({progress});
      },
      filter_modes);

  _state->view_data()->_glitch_filter_active = true;
  _state->view_data()->_glitch_filter_thresholds = thresholds;
  _state->view_data()->_glitch_filter_modes = filter_modes;

  _event_bus->broadcast_async<interface::GlitchFilterCompleted>({});
  _coord->data_updated();

  // M-1 fix: process pending requests in a LOOP within the SAME thread,
  // instead of recursively calling set_glitch_filter() (which created a
  // new thread and detached the current one, causing the old thread to
  // escape ownership and potentially access destroyed data after stop()).
  // By looping here, the thread object stays unique and joinable by stop().
  while (_has_pending_glitch.load()) {
    std::map<int, uint32_t> pend_th;
    std::map<int, GlitchFilterMode> pend_md;
    {
      std::lock_guard<std::mutex> lk(_pending_mutex);
      if (_has_pending_glitch.load()) {
        pend_th = std::move(_pending_glitch_thresholds);
        pend_md = std::move(_pending_glitch_modes);
        _pending_glitch_thresholds.clear();
        _pending_glitch_modes.clear();
        _has_pending_glitch.store(false);
      }
    }
    if (pend_th.empty())
      break;

    // Re-run the filter with the pending parameters directly — no new
    // thread, no recursive set_glitch_filter() call.
    if (_state->view_data()->_logic_backup) {
      _state->view_data()->get_logic()->copy_from(
          *_state->view_data()->_logic_backup);
    }
    if (_state->view_data()->get_logic()) {
      _state->view_data()->get_logic()->clear_filtered_ranges();
    }
    // Re-apply signal invert if active
    if (_state->view_data()->_signal_invert_active) {
      int ch_idx = 0;
      for (const GSList *l = _state->device_agent().get_channels(); l;
           l = l->next) {
        sr_channel *const probe = (sr_channel *)l->data;
        if (probe->type != SR_CHANNEL_LOGIC)
          continue;
        if (ch_idx < (int)_state->view_data()->_signal_invert_channels.size() &&
            _state->view_data()->_signal_invert_channels[ch_idx]) {
          _state->view_data()->get_logic()->invert_channel(probe->index);
        }
        ch_idx++;
      }
    }
    _state->view_data()->get_logic()->apply_glitch_filter_all(
        pend_th,
        [this](int progress) {
          _event_bus->broadcast_async<interface::GlitchFilterProgress>({progress});
        },
        pend_md);
    _state->view_data()->_glitch_filter_thresholds = pend_th;
    _state->view_data()->_glitch_filter_modes = pend_md;

    _event_bus->broadcast_async<interface::GlitchFilterCompleted>({});
    _coord->data_updated();
  }

  _glitch_filter_running = false;
}

void FilterProcessor::clear_glitch_filter() {
  if (_glitch_filter_running)
    return;

  if (!_state->view_data()->_glitch_filter_active)
    return;

  if (_state->view_data()->_logic_backup) {
    _state->view_data()->get_logic()->copy_from(
        *_state->view_data()->_logic_backup);
    // Track B3: unique_ptr reset() replaces manual delete
    _state->view_data()->_logic_backup.reset();
  }

  // 清除滤波后清空持久化区间，恢复原始数据无 overlay
  if (_state->view_data()->get_logic()) {
    _state->view_data()->get_logic()->clear_filtered_ranges();
  }

  _state->view_data()->_glitch_filter_active = false;
  _state->view_data()->_glitch_filter_thresholds.clear();
  _state->view_data()->_glitch_filter_modes.clear();

  _event_bus->broadcast_async<interface::GlitchFilterCleared>({});
  _coord->data_updated();
}

bool FilterProcessor::is_glitch_filter_active() {
  return _state->view_data()->_glitch_filter_active;
}

void FilterProcessor::set_signal_invert(const std::vector<bool> &channels) {
  // H2 fix: lock the launch mutex to prevent TOCTOU race on the launch path.
  std::lock_guard<std::mutex> launch_lk(_signal_invert_launch_mutex);

  if (_signal_invert_running)
    return;

  if (_state->view_data()->get_logic()->empty())
    return;

  bool has_invert = false;
  for (auto ch : channels) {
    if (ch) {
      has_invert = true;
      break;
    }
  }
  if (!has_invert)
    return;

  _signal_invert_running = true;
  _event_bus->broadcast_async<interface::SignalInvertStarted>({});

  if (_signal_invert_thread) {
    if (_signal_invert_thread->get_id() == std::this_thread::get_id()) {
      _signal_invert_thread->detach();
      _signal_invert_thread.reset();
    } else {
      if (_signal_invert_thread->joinable())
        _signal_invert_thread->join();
      _signal_invert_thread.reset();
    }
  }

  _signal_invert_thread =
      std::make_unique<std::thread>(
          &FilterProcessor::signal_invert_task, this, channels);
}

void FilterProcessor::signal_invert_task(const std::vector<bool> channels) {
  if (!_state->view_data()->_logic_backup) {
    // Track B3: use make_unique instead of raw new
    _state->view_data()->_logic_backup = std::make_unique<data::LogicSnapshot>();
    _state->view_data()->_logic_backup->copy_from(
        *(_state->view_data()->get_logic()));
    if (_state->view_data()->_logic_backup->memory_failed()) {
      _state->view_data()->_logic_backup.reset();
      _signal_invert_running = false;
      _event_bus->broadcast_async<interface::SignalInvertCompleted>({});
      return;
    }
  } else {
    _state->view_data()->get_logic()->copy_from(
        *_state->view_data()->_logic_backup);
  }

  // Apply invert on each enabled channel
  int ch_idx = 0;
  for (const GSList *l = _state->device_agent().get_channels(); l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;
    if (probe->type != SR_CHANNEL_LOGIC)
      continue;
    if (ch_idx < (int)channels.size() && channels[ch_idx]) {
      _state->view_data()->get_logic()->invert_channel(probe->index);
    }
    ch_idx++;
  }

  // If glitch filter is active, re-apply on the inverted data
  if (_state->view_data()->_glitch_filter_active) {
    _state->view_data()->get_logic()->apply_glitch_filter_all(
        _state->view_data()->_glitch_filter_thresholds, nullptr,
        _state->view_data()->_glitch_filter_modes);
  }

  _state->view_data()->_signal_invert_active = true;
  _state->view_data()->_signal_invert_channels = channels;
  _signal_invert_running = false;

  _event_bus->broadcast_async<interface::SignalInvertCompleted>({});
  _coord->data_updated();
}

void FilterProcessor::clear_signal_invert() {
  if (_signal_invert_running)
    return;

  if (!_state->view_data()->_signal_invert_active)
    return;

  if (_state->view_data()->_logic_backup) {
    _state->view_data()->get_logic()->copy_from(
        *_state->view_data()->_logic_backup);
    // Track B3: unique_ptr reset() replaces manual delete
    _state->view_data()->_logic_backup.reset();
  }

  // If glitch filter is active, re-apply on the restored (non-inverted) data
  if (_state->view_data()->_glitch_filter_active) {
    _state->view_data()->get_logic()->apply_glitch_filter_all(
        _state->view_data()->_glitch_filter_thresholds, nullptr,
        _state->view_data()->_glitch_filter_modes);
  }

  _state->view_data()->_signal_invert_active = false;
  _state->view_data()->_signal_invert_channels.clear();

  _event_bus->broadcast_async<interface::SignalInvertCleared>({});
  _coord->data_updated();
}

bool FilterProcessor::is_signal_invert_active() {
  return _state->view_data()->_signal_invert_active;
}

} // namespace core
} // namespace pv
