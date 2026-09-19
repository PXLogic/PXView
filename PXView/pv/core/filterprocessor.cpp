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

namespace {

// Minimum spacing between progressive-refresh publications during a filter
// pass (~6-7 fps). Fast enough to read as "live progress", slow enough that the
// GUI's data_updated() pass (layout + margins + scrollbars + signal-pixmap
// rebuild) is negligible next to the filter's own work — and, because each
// notice makes the renderer take the shared edit-read lock, it also bounds how
// often the writer can be made to wait for a paint.
constexpr int kFilterRefreshIntervalMs = 150;

// Upper bound on how long the *synchronous* clear variants (API/MCP, never the
// GUI) wait for their worker task. The undo is bounded by the edit log, i.e. by
// ~one copy of the channel data, so this is far more than a healthy case needs;
// it only turns a pathological hang into a warning.
//
// KNOWN NARROW EDGE (gap audit ⑤, accepted): since clears are queued while a
// pass runs, the synchronous variant may wait for "rest of the pass + undo".
// On timeout it RETURNS FIRST (the MCP tool still reports success) while the
// worker keeps going and restores the data shortly after — a client that reads
// samples immediately after a timeout warning may still see filtered data.
// Realistic cases fit easily (an undo of a full-capture filter is on the order
// of a few seconds: touched blocks x ~10-30 ms mipmap rebuild); raising the
// bound further would only extend a pathological block instead of fixing it.
constexpr int kClearTaskWaitSeconds = 120;

} // anonymous namespace

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
  // Drop a queued-but-unserviced apply request as well. The pool is about to be
  // shut down, so nothing would ever service it; leaving _has_pending_glitch
  // set would make a hypothetical in-process restart service a STALE request in
  // its very first pass. (Unreachable today — shutdown() kills the pool and
  // SigSession::Close() is terminal — but the cleanup is one line and keeps the
  // stopped state actually free of pending work.)
  {
    std::lock_guard<std::mutex> lk(_pending_mutex);
    _has_pending_glitch.store(false);
    _pending_glitch_thresholds.clear();
    _pending_glitch_modes.clear();
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
    // The queued apply is a glitch-filter writer intent: it must supersede a
    // glitch clear that was queued earlier (see _glitch_intent_seq).
    _glitch_intent_seq.fetch_add(1, std::memory_order_relaxed);
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
  _glitch_intent_seq.fetch_add(1, std::memory_order_relaxed);
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
    if (ch_idx < static_cast<int>(channels.size()) && channels[ch_idx]) {
      // Same progressive-refresh need as the glitch filter (see
      // notify_batch_committed): inverting a channel XORs every one of its leaf
      // blocks (2 MB each) and rebuilds each block's whole mipmap, which is
      // seconds on a large capture. invert_channel() publishes its edit
      // transaction per chunk of blocks, so this callback fires while the pass
      // runs and the render path — which skips its rebuild only while a
      // transaction is open — can show the result chunk by chunk. Throttled by
      // notify_batch_committed like every other edit notice.
      logic->invert_channel(m->index(), [this]() { notify_batch_committed(); });

      // Final notice for the channel: makes sure the completed state is
      // repainted even if the per-chunk notices were all throttled away.
      if (_coord)
        _coord->data_updated();
    }
    ch_idx++;
  }
}

void FilterProcessor::notify_batch_committed() {
  const auto now = std::chrono::steady_clock::now();
  if (_last_batch_refresh.time_since_epoch().count() != 0 &&
      now - _last_batch_refresh <
          std::chrono::milliseconds(kFilterRefreshIntervalMs)) {
    return;
  }
  _last_batch_refresh = now;

  // Async (broadcast_async) — safe from the filter worker thread, and it is
  // what marshals the repaint request to the GUI thread. The GUI's paint then
  // blocks at most one batch on EditReadGuard if the worker is mid-batch.
  if (_coord)
    _coord->data_updated();
}

bool FilterProcessor::rebuild_filtered_state(
    const std::map<int, uint32_t> &thresholds,
    const std::map<int, GlitchFilterMode> &filter_modes) {
  auto *logic = _state->view_data()->get_logic();
  if (!logic)
    return false;

  // New pass — publish the very first committed batch immediately instead of
  // honouring a stamp left over from the previous pass.
  _last_batch_refresh = std::chrono::steady_clock::time_point{};

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
  //
  // The revert can itself be long (it replays up to ~one copy of the channel
  // data and rebuilds a mipmap per touched block), so it publishes progressive
  // notices exactly like the write batches below.
  _last_batch_refresh = std::chrono::steady_clock::time_point{};
  if (!logic->revert_all_edits([this]() { notify_batch_committed(); })) {
    // The undo itself hit an allocation failure (re-materialising a collapsed
    // block). The snapshot is in a PARTIALLY restored state — building a new
    // invert+filter result on top of mixed data would be garbage-in-garbage-
    // out. Report it visibly and abort; a retry (once memory allows) starts
    // from the same revert.
    pxv_err("FilterProcessor: initial undo failed (OOM while re-materialising "
            "blocks); aborting the pass");
    if (_coord) {
      _coord->set_error(SessionStateContext::Malloc_err);
      _coord->session_error();
    }
    logic->clear_filtered_ranges();
    replay_decode();
    return false;
  }

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
        filter_modes, &_filter_cancel,
        // Progressive refresh: each committed batch is a new visible revision,
        // so ask the GUI to redraw (throttled). Without this the view keeps
        // showing the cached pre-filter pixmap for the whole pass.
        [this]() { notify_batch_committed(); });
  }

  if (_filter_cancel.load(std::memory_order_relaxed)) {
    // A capture/config boundary asked us to get out of the way (see
    // wait_idle). The result is partial by construction, so roll it back
    // rather than leaving a half-filtered snapshot behind.
    pxv_info("FilterProcessor: pass cancelled by a config boundary; "
             "rolling back to capture data");
    if (!logic->revert_all_edits([this]() { notify_batch_committed(); })) {
      pxv_err("FilterProcessor: cancelled pass's rollback undo failed (OOM); "
              "snapshot left partially filtered");
    }
    logic->clear_filtered_ranges();
    replay_decode();
    return false;
  }

  if (logic->edit_pass_aborted()) {
    // Roll back instead of publishing a half-applied pass. The old code kept
    // going and set _glitch_filter_active = true, so an out-of-memory filter
    // reported success and the UI showed a partially filtered waveform with
    // no indication anything had gone wrong. edit_pass_failed() (NOT the
    // inherited memory_failed()) is the right signal here: that one is the
    // capture-pipeline degradation flag and must not fail edit passes.
    pxv_err("FilterProcessor: glitch filter aborted (edit_pass_failed=%d, "
            "edit_log_overflow=%d); rolling back to capture data",
            static_cast<int>(logic->edit_pass_failed()), static_cast<int>(logic->edit_log_overflowed()));
    // Make the refusal USER-VISIBLE for BOTH failure kinds (they are the same
    // OOM family). Rolling back and only logging leaves the waveform exactly
    // as it was, which is indistinguishable from "the filter did nothing" —
    // that is precisely how the dense-glitch budget bug showed up in the
    // field ("the large file isn't updated after filtering").
    if (_coord) {
      _coord->set_error(SessionStateContext::Malloc_err);
      _coord->session_error();
    }
    if (!logic->revert_all_edits([this]() { notify_batch_committed(); })) {
      // The rollback itself could not finish; the snapshot stays partially
      // filtered. Nothing further can be done here — the next pass starts
      // with its own revert and reports honestly again.
      pxv_err("FilterProcessor: rollback undo also failed (OOM); snapshot "
              "left partially filtered");
    }
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
    // INVARIANT: a pass only ever runs with a BLOCK of real work, so claiming
    // "applied" here is honest. set_glitch_filter() rejects an empty or
    // all-zero threshold set before starting anything (its has_filter scan),
    // and the pending loop below breaks on an empty queued set — so a request
    // that would filter nothing never reaches this tail. (A non-empty set that
    // names only absent channels still lands here and reports as applied,
    // exactly like a pass that found no glitches to remove: the configuration
    // is in effect, which is what the badge means.)
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
      // Non-empty by construction (see the invariant note in the main tail).
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
  auto sub = submit_clear_glitch_filter();
  if (!sub.fut.valid())
    return;

  // The API/MCP contract is "cleared when this returns": a client that reads
  // samples next must see capture-original data, not the filter's. So this
  // variant waits for the worker. It is called from the API/MCP threads, never
  // from the GUI — the GUI uses request_clear_glitch_filter().
  //
  // The wait is bounded on purpose: a pathological case degrades to a warning
  // instead of hanging the caller, and the task runs to completion either way.
  // With queueing, a valid future now also covers "a pass was running at
  // submission": the future completes only after the parked clear task has
  // actually restored the data, instead of the old behaviour of returning
  // immediately with the samples still filtered.
  if (sub.fut.wait_for(std::chrono::seconds(kClearTaskWaitSeconds)) !=
      std::future_status::ready) {
    pxv_warn("FilterProcessor::clear_glitch_filter: undo still running after "
             "%d s; returning before it completes", kClearTaskWaitSeconds);
  }
}

bool FilterProcessor::request_clear_glitch_filter() {
  // Fire and forget as far as the caller (the GUI thread) is concerned.
  // Completion is announced by GlitchFilterCleared + DataUpdated, so the View's
  // refresh path is identical to the synchronous variant — only the blocking on
  // a multi-second undo is gone.
  //
  // The return value tells the View whether the request was parked behind a
  // running pass, so it can show "will run when the current pass finishes"
  // instead of a premature "cleared" toast. It comes from the submission
  // itself (no second read of the running flag — that could race with a pass
  // finishing in between and report the wrong thing).
  return submit_clear_glitch_filter().queued;
}

FilterProcessor::ClearSubmission FilterProcessor::submit_clear_glitch_filter() {
  // These checks run on the caller's (possibly GUI) thread, so they must stay
  // cheap and must NOT take _edit_mutex. The original version acquired the
  // edit lock first and only then tested whether a pass was in flight, so
  // clicking "clear / undo filter" during a pass blocked the caller for the
  // entire pass and *then* discovered it had nothing to do.
  std::lock_guard<std::mutex> launch_lk(_glitch_launch_mutex);

  const bool running = _glitch_filter_running.load();
  if (running) {
    // A clear during a running pass is QUEUED, not dropped: the pool task
    // parks on _edit_mutex and runs the undo right after the pass finishes.
    // Previously the request was silently discarded here and the user had to
    // click again — and the sync API/MCP variant even returned with the data
    // still filtered.
    //
    // Dropping a queued apply belongs to THIS branch only: otherwise the pass
    // loop would re-apply the filter immediately before our queued clear runs.
    std::lock_guard<std::mutex> lk(_pending_mutex);
    _has_pending_glitch.store(false);
    _pending_glitch_thresholds.clear();
    _pending_glitch_modes.clear();
  } else {
    // Nothing in flight: only a genuinely applied filter is worth an undo.
    // Read under the filter-state lock — that lock is the documented owner of
    // this flag, and a worker may be flipping it right now.
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    if (!_state->view_data()->_glitch_filter_active)
      return {};
  }

  const uint64_t seq =
      _glitch_intent_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  auto self = this;
  return ClearSubmission{
      _filter_pool.submit([self, seq]() {
        self->clear_glitch_filter_task(seq);
      }),
      running};
}

void FilterProcessor::clear_glitch_filter_task(uint64_t intent_seq) {
  // Worker thread: blocking on _edit_mutex here is correct and is the whole
  // point of the split — undoing a filter over a large capture takes seconds
  // (up to ~one copy of the channel data rewritten, plus one full mipmap
  // rebuild per touched block), and the GUI must not perform that work. When
  // the task was queued behind a running pass, this acquisition is exactly
  // where it waits for the pass to finish.
  std::lock_guard<std::timed_mutex> edit_lk(_edit_mutex);

  // Latest-writer-wins, per feature: if a newer GLITCH-FILTER intent (an apply
  // or another clear) was submitted after this clear was queued, this clear is
  // stale — running it would undo the user's newer instruction. The newer
  // intent produces the final state on its own (a queued apply is serviced by
  // the pass; a newer clear queued its own task). An unrelated signal-INVERT
  // submission does not invalidate this clear: the features are orthogonal and
  // the invert pass would otherwise re-apply the very filter being cleared.
  if (_glitch_intent_seq.load(std::memory_order_relaxed) != intent_seq) {
    pxv_info("FilterProcessor: queued clear superseded by a newer glitch-filter "
             "intent; skipping");
    return;
  }

  clear_glitch_filter_locked();
}

void FilterProcessor::clear_glitch_filter_locked() {
  // Re-check under the lock: the state may have changed between submission and
  // execution (a pass that finished meanwhile may have re-applied, or an
  // earlier queued clear already did the work). The clear is idempotent.
  // Read under the filter-state lock (its documented owner): the submission
  // path may flip this flag concurrently on another thread.
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    if (!_state->view_data()->_glitch_filter_active)
      return;
  }

  auto *logic = _state->view_data()->get_logic();
  bool revert_ok = true;
  if (logic) {
    // Undo the filter edits only; signal invert is a separate feature and
    // must survive a filter clear.
    //
    // The revert publishes progress notices (throttled by notify_batch_committed).
    _last_batch_refresh = std::chrono::steady_clock::time_point{};
    revert_ok = logic->revert_all_edits([this]() { notify_batch_committed(); });

    if (revert_ok) {
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
  }

  if (!revert_ok) {
    // The undo could not finish (OOM while re-materialising blocks): the
    // waveform is still (partially) filtered. Reporting "cleared" here would
    // be a lie — the old code cleared the active flag and broadcast
    // GlitchFilterCleared regardless, so the UI showed an unfiltered state
    // over filtered data. Keep the filter state active so the user can retry
    // once memory allows (the retry's revert re-resets the OOM scope), and
    // make the failure visible instead.
    pxv_err("FilterProcessor::clear_glitch_filter: undo failed (OOM while "
            "re-materialising blocks); filter state kept active for retry");
    if (_coord) {
      _coord->set_error(SessionStateContext::Malloc_err);
      _coord->session_error();
    }
    _event_bus->broadcast_async<interface::GlitchFilterCompleted>({});
    _coord->data_updated();
    return;
  }

  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    _state->view_data()->_glitch_filter_active = false;
    _state->view_data()->_glitch_filter_thresholds.clear();
    _state->view_data()->_glitch_filter_modes.clear();
  }

  // This path also rewrote samples in place (revert_all_edits), so the decode
  // output has to be re-derived. No pre-pause here on purpose: this rewrites the
  // data once and clear_all_decode_task2() JOINS the decode workers, so a
  // pause+replay pair would pay that join twice. The replay alone preserves the
  // previous semantics.
  if (_coord)
    _coord->restart_decode_tasks();

  _event_bus->broadcast_async<interface::GlitchFilterCleared>({});
  _coord->data_updated();
}

bool FilterProcessor::is_glitch_filter_active() {
  std::lock_guard<std::mutex> lk(_state->view_data()->_filter_state_mutex);
  return _state->view_data()->_glitch_filter_active;
}

void FilterProcessor::auto_apply_saved_filter() {
  // The enabled flag is atomic: no lock needed, and it lets the common
  // "auto-apply off" case exit before touching anything else.
  if (!_state->view_data()->_glitch_filter_auto_apply)
    return;

  // Copy the configuration out under its documented lock, then submit after
  // releasing it — set_glitch_filter() takes the same non-recursive lock.
  std::map<int, uint32_t> th;
  std::map<int, GlitchFilterMode> md;
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    th = _state->view_data()->_glitch_filter_thresholds;
    md = _state->view_data()->_glitch_filter_modes;
  }

  auto *logic = _state->view_data()->get_logic();
  if (th.empty() || !logic || logic->empty())
    return;   // nothing configured, or no data to filter yet

  set_glitch_filter(th, md);
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
  _invert_intent_seq.fetch_add(1, std::memory_order_relaxed);
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
  _last_batch_refresh = std::chrono::steady_clock::time_point{};
  if (!logic->revert_all_edits([this]() { notify_batch_committed(); })) {
    // Undo failed (OOM): the data is partially restored, so re-applying the
    // invert (and possibly the filter) on top would be garbage. Report and
    // bail — the user can retry, the retry's revert resets the OOM scope.
    pxv_err("FilterProcessor::signal_invert_task: initial undo failed (OOM); "
            "aborting the pass");
    if (_coord) {
      _coord->set_error(SessionStateContext::Malloc_err);
      _coord->session_error();
    }
    _signal_invert_running = false;
    _event_bus->broadcast_async<interface::SignalInvertCompleted>({});
    _coord->data_updated();
    return;
  }
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
  if (has_gf && !gf_th_copy.empty()) {
    // Same progressive refresh as rebuild_filtered_state (this re-apply is a
    // full pass in its own right and can take just as long).
    _last_batch_refresh = std::chrono::steady_clock::time_point{};
    logic->apply_glitch_filter_all(gf_th_copy, nullptr, gf_md_copy,
                                   nullptr,
                                   [this]() { notify_batch_committed(); });
  }

  if (logic->edit_pass_aborted()) {
    pxv_err("FilterProcessor::signal_invert_task: pass failed "
            "(edit_pass_failed=%d, edit_log_overflow=%d); rolling back",
            static_cast<int>(logic->edit_pass_failed()), static_cast<int>(logic->edit_log_overflowed()));
    // Same user-visible report as the glitch-filter pass: OOM-family
    // failures must not look like "the invert did nothing".
    if (_coord) {
      _coord->set_error(SessionStateContext::Malloc_err);
      _coord->session_error();
    }
    const bool rollback_ok =
        logic->revert_all_edits([this]() { notify_batch_committed(); });
    if (!rollback_ok) {
      // DOUBLE FAILURE (the pass OOM'd AND the rollback OOM'd): the snapshot
      // stays partially edited and the applied-state flags are deliberately
      // left AS-IS below — they still claim "applied", which is the truthful
      // reading of a half-edited store, and the user-visible Malloc_err was
      // already raised above. Nothing further can be done from here; the next
      // pass starts with its own revert_all_edits() (which re-resets the
      // edit-pass scope) and either finishes the restore or reports again.
      pxv_err("FilterProcessor::signal_invert_task: rollback undo also "
              "failed (OOM); snapshot left partially edited, applied-state "
              "flags kept as-is");
    }
    logic->clear_filtered_ranges();
    if (rollback_ok) {
      // The rollback restored capture-original data: NEITHER the invert nor
      // the glitch filter is applied anymore. Leaving the previous active
      // flags set would make the UI claim a filtered/inverted waveform over
      // unfiltered samples — reconcile the state with the data.
      //
      // Only the APPLIED state is cleared. `_glitch_filter_thresholds/_modes`
      // are deliberately KEPT: they are the user's filter CONFIGURATION, not
      // applied state — the convention the rest of the session already relies
      // on (SessionStateContext::clear_glitch_filter_state_for_capture and
      // SigSession::restore_glitch_filter_config both clear only `_active` and
      // keep the config so the auto-apply path can re-apply it). Clearing them
      // here would reset the filter panel's sliders after a transient OOM.
      // `_signal_invert_channels` has no such config role (there is no invert
      // auto-apply), so it goes with the applied state.
      std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
      _state->view_data()->_signal_invert_active = false;
      _state->view_data()->_signal_invert_channels.clear();
      _state->view_data()->_glitch_filter_active = false;
    }
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
  auto sub = submit_clear_signal_invert();
  if (!sub.fut.valid())
    return;

  // Same "cleared means cleared" contract as clear_glitch_filter(): the API/MCP
  // caller waits (on its own thread), the GUI does not.
  if (sub.fut.wait_for(std::chrono::seconds(kClearTaskWaitSeconds)) !=
      std::future_status::ready) {
    pxv_warn("FilterProcessor::clear_signal_invert: undo still running after "
             "%d s; returning before it completes", kClearTaskWaitSeconds);
  }
}

bool FilterProcessor::request_clear_signal_invert() {
  // Fire and forget for the GUI thread. See request_clear_glitch_filter().
  // `queued` comes from the submission itself (single read of the running flag).
  return submit_clear_signal_invert().queued;
}

FilterProcessor::ClearSubmission FilterProcessor::submit_clear_signal_invert() {
  // Same cheap-checks-on-caller-thread ordering as submit_clear_glitch_filter().
  // A clear during a running invert pass is QUEUED (pool task parks on
  // _edit_mutex), never silently dropped. Unlike the glitch variant there is
  // no pending-apply queue to drop here: invert submissions are never parked,
  // so a running pass can only be superseded by a whole new invert task.
  std::lock_guard<std::mutex> launch_lk(_signal_invert_launch_mutex);

  const bool running = _signal_invert_running.load();
  {
    std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
    if (!_state->view_data()->_signal_invert_active)
      return {};
  }

  const uint64_t seq =
      _invert_intent_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  auto self = this;
  return ClearSubmission{
      _filter_pool.submit([self, seq]() {
        self->clear_signal_invert_task(seq);
      }),
      running};
}

void FilterProcessor::clear_signal_invert_task(uint64_t intent_seq) {
  // Worker thread — see clear_glitch_filter_task() for why blocking is right
  // here and wrong on the GUI thread.
  std::lock_guard<std::timed_mutex> edit_lk(_edit_mutex);

  // Latest-writer-wins, per feature: only a newer INVERT intent invalidates
  // this clear (a glitch-filter submission is unrelated and must not cancel
  // the invert the user asked to clear). See clear_glitch_filter_task().
  if (_invert_intent_seq.load(std::memory_order_relaxed) != intent_seq) {
    pxv_info("FilterProcessor: queued invert-clear superseded by a newer "
             "signal-invert intent; skipping");
    return;
  }

  clear_signal_invert_locked();
}

void FilterProcessor::clear_signal_invert_locked() {
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
    return;   // re-checked under the lock: idempotent

  auto *logic = _state->view_data()->get_logic();
  bool revert_ok = true;
  if (logic) {
    // Progressive notices for the undo (see notify_batch_committed).
    _last_batch_refresh = std::chrono::steady_clock::time_point{};
    revert_ok = logic->revert_all_edits([this]() { notify_batch_committed(); });

    if (revert_ok) {
      // If glitch filter is active, re-apply it on the restored
      // (non-inverted) data. This is a full pass and gets the same treatment.
      if (gf_active && !gf_th.empty()) {
        logic->apply_glitch_filter_all(gf_th, nullptr, gf_md, nullptr,
                                       [this]() { notify_batch_committed(); });
      }

      if (logic->edit_pass_aborted()) {
        pxv_err("FilterProcessor::clear_signal_invert: re-filter failed; "
                "rolling back");
        if (_coord) {
          _coord->set_error(SessionStateContext::Malloc_err);
          _coord->session_error();
        }
        const bool rollback_ok =
            logic->revert_all_edits([this]() { notify_batch_committed(); });
        if (!rollback_ok) {
          // DOUBLE FAILURE (re-filter OOM'd AND the rollback OOM'd): the
          // snapshot stays partially edited and the applied-state flags are
          // deliberately left AS-IS below (they still claim "applied"), which
          // is the truthful reading of a half-edited store; the user-visible
          // Malloc_err already fired above. The next clear/pass starts with
          // its own revert_all_edits() and either finishes the restore or
          // reports again.
          pxv_err("FilterProcessor::clear_signal_invert: rollback undo also "
                  "failed (OOM); snapshot left partially edited, applied-state "
                  "flags kept as-is");
        }
        logic->clear_filtered_ranges();
        if (rollback_ok) {
          // The rollback restored capture-original data: the glitch filter
          // did NOT survive (the re-apply failed and was undone). Clearing
          // only the invert state here would leave _glitch_filter_active
          // claiming a filtered waveform over unfiltered samples.
          //
          // Clear the APPLIED flag only; keep the user's threshold/mode
          // CONFIG — same convention as signal_invert_task() and as
          // SessionStateContext::clear_glitch_filter_state_for_capture (config
          // is kept for the auto-apply path; only `_active` describes whether
          // it is applied to the current data).
          std::lock_guard<std::mutex> flk(_state->view_data()->_filter_state_mutex);
          _state->view_data()->_glitch_filter_active = false;
        }
      }
    }
  }

  if (!revert_ok) {
    // Undo failed (OOM): the invert is NOT cleared — the data is still
    // (partially) inverted. Keep the state active for a retry and report
    // visibly instead of broadcasting a false "cleared".
    pxv_err("FilterProcessor::clear_signal_invert: undo failed (OOM while "
            "re-materialising blocks); invert state kept active for retry");
    if (_coord) {
      _coord->set_error(SessionStateContext::Malloc_err);
      _coord->session_error();
    }
    _event_bus->broadcast_async<interface::SignalInvertCompleted>({});
    _coord->data_updated();
    return;
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
