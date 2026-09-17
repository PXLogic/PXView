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

bool FilterProcessor::wait_idle(int max_wait_ms) {
  // Bounded wait that actually honours its argument. The previous
  // implementation was declared `void wait_idle(int)` — the parameter was
  // dropped on the floor — and called the unbounded
  // ThreadPool::wait_for_idle(). Callers therefore blocked the calling thread
  // for the entire duration of a running pass, and init_signals() (i.e. every
  // samplerate change, re-capture and device switch) is a GUI-thread caller:
  // the event loop stopped for as long as the filter ran.
  //
  // The ThreadPool join is still the right primitive for the lifetime
  // guarantee (it covers the submit->execution window that a running-flag
  // poll cannot), it just has to be time-bounded. See the note in the header:
  // on timeout the caller must NOT tear down the snapshot.
  // Ask any in-flight pass to stop at its next scan iteration BEFORE waiting.
  // Bounding the wait alone would only convert an unbounded freeze into a
  // shorter one — the boundary would still sit through the remainder of the
  // pass. With cancellation the scan bails out at the next iteration, so the
  // wait collapses to near-zero. Cleared afterwards so subsequent passes run
  // to completion.
  _filter_cancel.store(true, std::memory_order_relaxed);
  const int bounded_ms = max_wait_ms > 0 ? max_wait_ms : 0;
  const bool idle = _filter_pool.wait_for_idle_for(
      std::chrono::milliseconds(bounded_ms));
  _filter_cancel.store(false, std::memory_order_relaxed);

  if (!idle) {
    pxv_warn("FilterProcessor::wait_idle: filter pool still busy after %d ms; "
             "caller must defer the snapshot rebuild",
             bounded_ms);
  }
  return idle;
}

void FilterProcessor::set_glitch_filter(
    const std::map<int, uint32_t> &thresholds,
    const std::map<int, GlitchFilterMode> &filter_modes) {
  // S1/H2 fix: lock the launch mutex for the entire check-and-create path.
  // This prevents two callers from both seeing _running==false and creating
  // duplicate tasks.
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

  auto *logic = _state->view_data()->get_logic();
  if (!logic || logic->empty())
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

void FilterProcessor::apply_signal_invert(data::LogicSnapshot *logic,
                                          const std::vector<bool> &channels) {
  if (!logic)
    return;

  int ch_idx = 0;
  // Iterate SignalModels (the source of truth for channel metadata). The
  // LOGIC sub-sequence order matches get_channels() (both ordered by index),
  // so the ch_idx → channels index correspondence is preserved.
  for (auto &m : _state->signal_models()) {
    if (!m || m->type() != SR_CHANNEL_LOGIC)
      continue;
    if (ch_idx < (int)channels.size() && channels[ch_idx])
      logic->invert_channel(m->index());
    ch_idx++;
  }
}

bool FilterProcessor::rebuild_filtered_state(
    const std::map<int, uint32_t> &thresholds,
    const std::map<int, GlitchFilterMode> &filter_modes) {
  auto *logic = _state->view_data()->get_logic();
  if (!logic)
    return false;

  // STOP THE DECODERS FIRST, before a single sample is rewritten.
  //
  // Decoders read these very leaf blocks (through get_samples() and the
  // SegmentDataIterator protocol, which hands out a raw pointer the decoder
  // keeps using after the call returns — so no per-call lock can protect it).
  // With the edit running concurrently a decoder splices pre- and post-edit
  // samples into one annotation. The results are discarded anyway by the
  // post-edit re-decode, but the ordering here removes the window entirely and
  // stops burning decoder CPU on data that is about to be invalidated.
  //
  // clear_all_decode_task2() joins the decode workers. That is deliberate and
  // safe on this (worker) thread: DataFeedParser calls the same function from
  // the datafeed thread for exactly this "stop before the snapshot changes"
  // reason, and decode workers take none of the locks this pass holds.
  //
  // The matching replay is issued as an explicit COMMAND before this function
  // returns (see replay_decode below), and therefore BEFORE the
  // GlitchFilter*/SignalInvert* notices are broadcast — so those events stay
  // pure notices and their subscribers observe a consistent state (AGENTS.md
  // Command/Notice split). It is Core-side, so it also happens in pxviewd/MCP
  // sessions that have no View.
  if (_coord)
    _coord->clear_all_decode_task2();

  // The pause above MUST be balanced by a replay on every exit path below, or
  // the decoders stay stopped for the rest of the session.
  auto replay_decode = [this]() {
    if (_coord)
      _coord->restart_decode_tasks();
  };

  // Belt and braces for any reader that is NOT a decode worker: drain the
  // sample iterators so nothing can still be holding a block pointer. Bounded
  // and best-effort — the decoder stop above is what actually guarantees it.
  if (!logic->wait_active_iterators_zero(500)) {
    pxv_warn("FilterProcessor: %d sample iterator(s) still active after 500 ms; "
             "proceeding with the edit pass", logic->active_iterator_count());
  }

  // Roll the snapshot back to capture-original (undoing the previous glitch
  // filter and/or signal invert), then rebuild the target state.
  //
  // This replaces copy_from(_logic_backup). The backup snapshot existed only
  // to make the filter reversible, and cost a second full copy of the sample
  // store — 976 MB for 8 channels x 1 GS/s, plus a complete MmapAllocator
  // reset/unmap (480 MEM_COMMIT calls, all pages faulted back in) on every
  // re-filter, i.e. on every threshold slider move. revert_all_edits() costs
  // O(bytes actually rewritten) instead, and touches no allocator state at
  // all, which is also what keeps the lock-free finite-capture readers'
  // "no block is ever freed" invariant true.
  logic->revert_all_edits();

  // Signal invert is applied BEFORE the glitch filter, matching the previous
  // ordering (the two compose as `filter(invert(raw))`).
  bool has_invert = false;
  std::vector<bool> channels_copy;
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    has_invert = _state->view_data()->_signal_invert_active;
    channels_copy = _state->view_data()->_signal_invert_channels;
  }
  if (has_invert)
    apply_signal_invert(logic, channels_copy);

  if (!thresholds.empty()) {
    logic->apply_glitch_filter_all(
        thresholds,
        [this](int progress) {
          _event_bus->broadcast_async<interface::GlitchFilterProgress>({progress});
        },
        filter_modes, &_filter_cancel);
  }

  if (_filter_cancel.load(std::memory_order_relaxed)) {
    // A capture/config boundary asked us to get out of the way (see
    // wait_idle). The result is partial by construction, so roll it back
    // rather than leaving a half-filtered snapshot behind.
    pxv_info("FilterProcessor: pass cancelled by a config boundary; "
             "rolling back to capture data");
    logic->revert_all_edits();
    logic->clear_filtered_ranges();
    replay_decode();
    return false;
  }

  if (logic->memory_failed() || logic->edit_log_overflowed()) {
    // Roll back instead of publishing a half-applied pass. The old code kept
    // going and set _glitch_filter_active = true, so an out-of-memory filter
    // reported success and the UI showed a partially filtered waveform with
    // no indication anything had gone wrong.
    pxv_err("FilterProcessor: glitch filter aborted (memory_failed=%d, "
            "edit_log_overflow=%d); rolling back to capture data",
            (int)logic->memory_failed(), (int)logic->edit_log_overflowed());
    logic->revert_all_edits();
    logic->clear_filtered_ranges();
    replay_decode();
    return false;
  }

  // Data is in its final state: re-derive the decode output now, while the
  // caller still has not broadcast its notice.
  replay_decode();
  return true;
}

void FilterProcessor::glitch_filter_task(
    const std::map<int, uint32_t> thresholds,
    const std::map<int, GlitchFilterMode> filter_modes) {
  // Serialize against clear_glitch_filter / signal_invert (see _edit_mutex).
  // Held for the whole task so the revert+rebuild sequence is atomic with
  // respect to other writers.
  std::lock_guard<std::timed_mutex> edit_lk(_edit_mutex);

  if (!rebuild_filtered_state(thresholds, filter_modes)) {
    {
      std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
      _state->view_data()->_glitch_filter_active = false;
    }
    _glitch_filter_running = false;
    _event_bus->broadcast_async<interface::GlitchFilterCompleted>({});
    _coord->data_updated();
    return;
  }

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

    // Re-run directly from the capture-original state — no backup copy, no
    // new thread, no recursive set_glitch_filter() call.
    if (!rebuild_filtered_state(pend_th, pend_md)) {
      std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
      _state->view_data()->_glitch_filter_active = false;
      _glitch_filter_running = false;
      _event_bus->broadcast_async<interface::GlitchFilterCompleted>({});
      _coord->data_updated();
      return;
    }

    {
      std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
      _state->view_data()->_glitch_filter_active = true;
      _state->view_data()->_glitch_filter_thresholds = pend_th;
      _state->view_data()->_glitch_filter_modes = pend_md;
    }

    _event_bus->broadcast_async<interface::GlitchFilterCompleted>({});
    _coord->data_updated();
  }

  _glitch_filter_running = false;
}

void FilterProcessor::clear_glitch_filter() {
  // GUARD ORDER MATTERS. The running check must come before the lock, not
  // after it. The previous version took _backup_mutex first and only then
  // tested _glitch_filter_running, so clicking "clear / undo filter" while a
  // pass was in flight blocked the GUI thread for the entire pass and *then*
  // discovered it was running and returned having done nothing. From the
  // user's point of view the button froze the application for seconds and
  // then had no effect.
  if (_glitch_filter_running.load())
    return;

  // Even now the lock acquisition is time-bounded: if another writer holds it
  // we give up rather than stall the event loop. The operation is idempotent
  // and user-retryable, so "did nothing" is a strictly better failure mode
  // than "window is not responding".
  std::unique_lock<std::timed_mutex> edit_lk(_edit_mutex, std::defer_lock);
  if (!edit_lk.try_lock_for(std::chrono::milliseconds(kEditLockWaitMs))) {
    pxv_warn("FilterProcessor::clear_glitch_filter: another writer holds the "
             "edit lock; skipping this request");
    return;
  }

  if (!_state->view_data()->_glitch_filter_active)
    return;

  auto *logic = _state->view_data()->get_logic();
  if (logic) {
    // Undo the filter edits only; signal invert is a separate feature and
    // must survive a filter clear.
    logic->revert_all_edits();

    bool has_invert = false;
    std::vector<bool> channels_copy;
    {
      std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
      has_invert = _state->view_data()->_signal_invert_active;
      channels_copy = _state->view_data()->_signal_invert_channels;
    }
    if (has_invert)
      apply_signal_invert(logic, channels_copy);

    // 清除滤波后清空持久化区间，恢复原始数据无 overlay
    logic->clear_filtered_ranges();
  }

  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    _state->view_data()->_glitch_filter_active = false;
    _state->view_data()->_glitch_filter_thresholds.clear();
    _state->view_data()->_glitch_filter_modes.clear();
  }

  // This path also rewrote samples in place (revert_all_edits), so the decode
  // output has to be re-derived. No pre-pause here on purpose: this runs on the
  // GUI thread and clear_all_decode_task2() JOINS the decode workers, so a
  // pause+replay pair would pay that join twice. The replay alone preserves the
  // previous semantics (the View used to call restart_decoders() from this same
  // thread, which joins identically).
  if (_coord)
    _coord->restart_decode_tasks();

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

  auto *logic = _state->view_data()->get_logic();
  if (!logic || logic->empty())
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
  // Serialize with glitch/clear (see _edit_mutex).
  std::lock_guard<std::timed_mutex> edit_lk(_edit_mutex);

  auto *logic = _state->view_data()->get_logic();
  if (!logic) {
    _signal_invert_running = false;
    _event_bus->broadcast_async<interface::SignalInvertCompleted>({});
    return;
  }

  // Same "stop before rewriting" ordering as the glitch-filter task: this pass
  // rewrites whole channels in place (invert), so the decoders must not still
  // be reading them. Balanced by restart_decode_tasks() on every exit below.
  if (_coord)
    _coord->clear_all_decode_task2();

  // Back to capture-original, then re-apply invert, then re-apply the glitch
  // filter if it is active — same composition as before, without the backup.
  logic->revert_all_edits();
  apply_signal_invert(logic, channels);

  // If glitch filter is active, re-apply on the inverted data.
  bool has_gf = false;
  std::map<int, uint32_t> gf_th_copy;
  std::map<int, GlitchFilterMode> gf_md_copy;
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    has_gf = _state->view_data()->_glitch_filter_active;
    gf_th_copy = _state->view_data()->_glitch_filter_thresholds;
    gf_md_copy = _state->view_data()->_glitch_filter_modes;
  }
  if (has_gf && !gf_th_copy.empty())
    logic->apply_glitch_filter_all(gf_th_copy, nullptr, gf_md_copy);

  if (logic->memory_failed() || logic->edit_log_overflowed()) {
    pxv_err("FilterProcessor::signal_invert_task: pass failed "
            "(memory_failed=%d, edit_log_overflow=%d); rolling back",
            (int)logic->memory_failed(), (int)logic->edit_log_overflowed());
    logic->revert_all_edits();
    logic->clear_filtered_ranges();
    if (_coord)
      _coord->restart_decode_tasks();
    _signal_invert_running = false;
    _event_bus->broadcast_async<interface::SignalInvertCompleted>({});
    _coord->data_updated();
    return;
  }

  // Lock to safely write _signal_invert_active/_signal_invert_channels.
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    _state->view_data()->_signal_invert_active = true;
    _state->view_data()->_signal_invert_channels = channels;
  }
  _signal_invert_running = false;

  // Explicit command BEFORE the notice (Command/Notice split).
  if (_coord)
    _coord->restart_decode_tasks();

  _event_bus->broadcast_async<interface::SignalInvertCompleted>({});
  _coord->data_updated();
}

void FilterProcessor::clear_signal_invert() {
  // Same guard-before-lock ordering as clear_glitch_filter().
  if (_signal_invert_running.load())
    return;

  std::unique_lock<std::timed_mutex> edit_lk(_edit_mutex, std::defer_lock);
  if (!edit_lk.try_lock_for(std::chrono::milliseconds(kEditLockWaitMs))) {
    pxv_warn("FilterProcessor::clear_signal_invert: another writer holds the "
             "edit lock; skipping this request");
    return;
  }

  bool invert_active = false;
  bool gf_active = false;
  std::map<int, uint32_t> gf_th;
  std::map<int, GlitchFilterMode> gf_md;
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    invert_active = _state->view_data()->_signal_invert_active;
    gf_active = _state->view_data()->_glitch_filter_active;
    gf_th = _state->view_data()->_glitch_filter_thresholds;
    gf_md = _state->view_data()->_glitch_filter_modes;
  }
  if (!invert_active)
    return;

  auto *logic = _state->view_data()->get_logic();
  if (logic) {
    logic->revert_all_edits();

    // If glitch filter is active, re-apply it on the restored
    // (non-inverted) data.
    if (gf_active && !gf_th.empty())
      logic->apply_glitch_filter_all(gf_th, nullptr, gf_md);

    if (logic->memory_failed() || logic->edit_log_overflowed()) {
      pxv_err("FilterProcessor::clear_signal_invert: re-filter failed; "
              "rolling back");
      logic->revert_all_edits();
      logic->clear_filtered_ranges();
    }
  }

  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    _state->view_data()->_signal_invert_active = false;
    _state->view_data()->_signal_invert_channels.clear();
  }

  // Same as clear_glitch_filter(): samples were rewritten in place, so re-derive
  // the decode output — as an explicit command before the notice, and without a
  // redundant pre-pause (see the note there).
  if (_coord)
    _coord->restart_decode_tasks();

  _event_bus->broadcast_async<interface::SignalInvertCleared>({});
  _coord->data_updated();
}

bool FilterProcessor::is_signal_invert_active() {
  std::lock_guard<std::mutex> lk(_state->view_data()->_filter_state_mutex);
  return _state->view_data()->_signal_invert_active;
}

} // namespace core
} // namespace pv
