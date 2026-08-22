#include "pv/core/filterprocessor.h"

#include "pv/core/eventbus.h"
#include "pv/core/sessionstatecontext.h"
#include "pv/session/sigsession.h"  // SessionData full definition
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/base/log.h"

#include <chrono>
#include <thread>
#include <libsigrok/libsigrok.h>

namespace pv {
namespace core {

FilterProcessor::FilterProcessor(EventBus *bus, ISessionState *state, ISessionCoordination *coord)
    : _event_bus(bus), _state(state), _coord(coord),
      _glitch_filter_running(false),
      _signal_invert_running(false) {}

FilterProcessor::~FilterProcessor() { stop(); }

void FilterProcessor::stop() {
  // Gap 1: ThreadPool::shutdown() joins all worker threads.
  // No need for manual join/detach or self-join detection.
  {
    std::lock_guard<std::mutex> lk(_glitch_launch_mutex);
    _glitch_filter_running = false;
  }
  {
    std::lock_guard<std::mutex> lk(_signal_invert_launch_mutex);
    _signal_invert_running = false;
  }
  _filter_pool.shutdown();
}

void FilterProcessor::wait_idle(int) {
  // [lifetime discipline] Delegate join to the worker pool. wait_for_idle()
  // blocks until every queued/running glitch/invert task has truly finished,
  // which is more reliable than polling a running flag: it covers the
  // submit->execution window and cannot wedge if a task faults before clearing
  // its flag. This is the Krita-style "join before teardown" guarantee that
  // closes the group3 test_36 crash (background copier vs snapshot rebuild).
  _filter_pool.wait_for_idle();
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

  // Gap 1: submit to ThreadPool instead of creating raw std::thread.
  // No self-join detection needed — ThreadPool workers are always
  // different threads from the caller.
  auto self = this;
  _filter_pool.submit([self, thresholds, filter_modes]() {
    self->glitch_filter_task(thresholds, filter_modes);
  });
}

void FilterProcessor::glitch_filter_task(
    const std::map<int, uint32_t> thresholds,
    const std::map<int, GlitchFilterMode> filter_modes) {
  // [group3 crash fix] Serialize against clear_glitch_filter / signal_invert
  // (see _backup_mutex). Holds for the whole task so copy_from(live<->backup)
  // and apply on the live snapshot cannot race a concurrent restore/clear.
  std::lock_guard<std::mutex> backup_lk(_backup_mutex);

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

  // If signal invert is active, apply invert before glitch filter.
  // Lock to safely read _signal_invert_active and _signal_invert_channels,
  // then release before the slow invert_channel loop.
  bool has_invert = false;
  std::vector<bool> channels_copy;
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    has_invert = _state->view_data()->_signal_invert_active;
    channels_copy = _state->view_data()->_signal_invert_channels;
  }
  if (has_invert) {
    int ch_idx = 0;
    for (const GSList *l = _state->device_agent().get_channels(); l;
         l = l->next) {
      sr_channel *const probe = (sr_channel *)l->data;
      if (probe->type != SR_CHANNEL_LOGIC)
        continue;
      if (ch_idx < (int)channels_copy.size() &&
          channels_copy[ch_idx]) {
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

  // Lock to safely write _glitch_filter_active/_thresholds/_modes —
  // the main thread (View layer) may concurrently read these for rendering.
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    _state->view_data()->_glitch_filter_active = true;
    _state->view_data()->_glitch_filter_thresholds = thresholds;
    _state->view_data()->_glitch_filter_modes = filter_modes;
  }

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
  // [group3 crash fix] Serialize with glitch/invert tasks (copy_from on the
  // live<->backup pair); see _backup_mutex.
  std::lock_guard<std::mutex> backup_lk(_backup_mutex);

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
  std::lock_guard<std::mutex> lk(_state->view_data()->_filter_state_mutex);
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

  // Gap 1: submit to ThreadPool.
  auto self = this;
  _filter_pool.submit([self, channels]() {
    self->signal_invert_task(channels);
  });
}

void FilterProcessor::signal_invert_task(const std::vector<bool> channels) {
  // [group3 crash fix] Serialize with glitch/clear (copy_from on the
  // live<->backup pair); see _backup_mutex.
  std::lock_guard<std::mutex> backup_lk(_backup_mutex);

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

  // If glitch filter is active, re-apply on the inverted data.
  // Lock to safely read glitch filter state, then release before the
  // slow apply_glitch_filter_all call.
  bool has_gf = false;
  std::map<int, uint32_t> gf_th_copy;
  std::map<int, GlitchFilterMode> gf_md_copy;
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    has_gf = _state->view_data()->_glitch_filter_active;
    gf_th_copy = _state->view_data()->_glitch_filter_thresholds;
    gf_md_copy = _state->view_data()->_glitch_filter_modes;
  }
  if (has_gf) {
    _state->view_data()->get_logic()->apply_glitch_filter_all(
        gf_th_copy, nullptr, gf_md_copy);
  }

  // Lock to safely write _signal_invert_active/_signal_invert_channels.
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    _state->view_data()->_signal_invert_active = true;
    _state->view_data()->_signal_invert_channels = channels;
  }
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
  std::lock_guard<std::mutex> lk(_state->view_data()->_filter_state_mutex);
  return _state->view_data()->_signal_invert_active;
}

} // namespace core
} // namespace pv
