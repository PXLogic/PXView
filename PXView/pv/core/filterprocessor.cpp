#include "filterprocessor.h"

#include "eventbus.h"
#include "sessionstatecontext.h"
#include "../sigsession.h"  // SessionData full definition
#include "../data/logicsnapshot.h"
#include "../log.h"

#include <libsigrok/libsigrok.h>

namespace pv {
namespace core {

FilterProcessor::FilterProcessor(EventBus *bus, SessionStateContext *state)
    : _event_bus(bus), _state(state),
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
  _glitch_filter_running = false;
  if (_glitch_filter_thread) {
    if (_glitch_filter_thread->joinable())
      _glitch_filter_thread->join();
    _glitch_filter_thread.reset();
  }
  _signal_invert_running = false;
  if (_signal_invert_thread) {
    if (_signal_invert_thread->joinable())
      _signal_invert_thread->join();
    _signal_invert_thread.reset();
  }
}

void FilterProcessor::set_glitch_filter(
    const std::map<int, uint32_t> &thresholds,
    const std::map<int, GlitchFilterMode> &filter_modes) {
  if (_glitch_filter_running) {
    // 架构修复：不再静默丢弃，排队最近一次请求，滤波完成后自动执行
    _pending_glitch_thresholds = thresholds;
    _pending_glitch_modes = filter_modes;
    _has_pending_glitch = true;
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

  if (_glitch_filter_thread) {
    _glitch_filter_thread->join();
    _glitch_filter_thread.reset();
  }

  _glitch_filter_thread = std::make_unique<std::thread>(
      &FilterProcessor::glitch_filter_task, this, thresholds, filter_modes);
}

void FilterProcessor::glitch_filter_task(
    const std::map<int, uint32_t> thresholds,
    const std::map<int, GlitchFilterMode> filter_modes) {
  if (!_state->view_data()->_logic_backup) {
    _state->view_data()->_logic_backup = new data::LogicSnapshot();
    _state->view_data()->_logic_backup->copy_from(
        *(_state->view_data()->get_logic()));
    if (_state->view_data()->_logic_backup->memory_failed()) {
      delete _state->view_data()->_logic_backup;
      _state->view_data()->_logic_backup = nullptr;
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
  _glitch_filter_running = false;

  _event_bus->broadcast_async<interface::GlitchFilterCompleted>({});
  _state->data_updated();

  // 架构修复：如果有排队的 pending 请求，立即执行
  if (_has_pending_glitch) {
    _has_pending_glitch = false;
    auto pend_th = std::move(_pending_glitch_thresholds);
    auto pend_md = std::move(_pending_glitch_modes);
    _pending_glitch_thresholds.clear();
    _pending_glitch_modes.clear();
    set_glitch_filter(pend_th, pend_md);
  }
}

void FilterProcessor::clear_glitch_filter() {
  if (_glitch_filter_running)
    return;

  if (!_state->view_data()->_glitch_filter_active)
    return;

  if (_state->view_data()->_logic_backup) {
    _state->view_data()->get_logic()->copy_from(
        *_state->view_data()->_logic_backup);
    delete _state->view_data()->_logic_backup;
    _state->view_data()->_logic_backup = nullptr;
  }

  // 清除滤波后清空持久化区间，恢复原始数据无 overlay
  if (_state->view_data()->get_logic()) {
    _state->view_data()->get_logic()->clear_filtered_ranges();
  }

  _state->view_data()->_glitch_filter_active = false;
  _state->view_data()->_glitch_filter_thresholds.clear();
  _state->view_data()->_glitch_filter_modes.clear();

  _event_bus->broadcast_async<interface::GlitchFilterCleared>({});
  _state->data_updated();
}

bool FilterProcessor::is_glitch_filter_active() {
  return _state->view_data()->_glitch_filter_active;
}

void FilterProcessor::set_signal_invert(const std::vector<bool> &channels) {
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
    _signal_invert_thread->join();
    _signal_invert_thread.reset();
  }

  _signal_invert_thread =
      std::make_unique<std::thread>(
          &FilterProcessor::signal_invert_task, this, channels);
}

void FilterProcessor::signal_invert_task(const std::vector<bool> channels) {
  if (!_state->view_data()->_logic_backup) {
    _state->view_data()->_logic_backup = new data::LogicSnapshot();
    _state->view_data()->_logic_backup->copy_from(
        *(_state->view_data()->get_logic()));
    if (_state->view_data()->_logic_backup->memory_failed()) {
      delete _state->view_data()->_logic_backup;
      _state->view_data()->_logic_backup = nullptr;
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
  _state->data_updated();
}

void FilterProcessor::clear_signal_invert() {
  if (_signal_invert_running)
    return;

  if (!_state->view_data()->_signal_invert_active)
    return;

  if (_state->view_data()->_logic_backup) {
    _state->view_data()->get_logic()->copy_from(
        *_state->view_data()->_logic_backup);
    delete _state->view_data()->_logic_backup;
    _state->view_data()->_logic_backup = nullptr;
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
  _state->data_updated();
}

bool FilterProcessor::is_signal_invert_active() {
  return _state->view_data()->_signal_invert_active;
}

} // namespace core
} // namespace pv
