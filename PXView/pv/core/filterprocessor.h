#ifndef PXVIEW_CORE_FILTERPROCESSOR_H
#define PXVIEW_CORE_FILTERPROCESSOR_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "pv/core/thread_pool.h"

// Spec v2 Task 4/5: GlitchFilterMode is now an enum class with fixed
// underlying type (int), so it can be forward-declared here without
// including the heavy logicsnapshot.h header.
enum class GlitchFilterMode : int;

#include "pv/core/isession_coordination.h"
#include "pv/core/isession_state.h"
#include "pv/core/isession_state.h"

namespace pv {

namespace data {
class LogicSnapshot;
} // namespace data

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

  /// Synchronous clear: returns only once capture-original data has actually
  /// been restored. This is the API/MCP contract — the returned "cleared" must
  /// mean the samples are already back, or a client that reads them next gets
  /// filtered data. The wait happens on the caller's own (non-GUI) thread.
  ///
  /// GUI callers MUST use request_clear_glitch_filter(): the undo rewrites up
  /// to ~one copy of the channel data and rebuilds the mipmap of every touched
  /// block, so running it inline froze the window for the whole undo.
  void clear_glitch_filter();

  /// Asynchronous clear for the GUI thread: submits the undo to the worker pool
  /// and returns immediately. Completion is announced by GlitchFilterCleared +
  /// DataUpdated, exactly as on the synchronous path, so the View/toast/repaint
  /// behaviour is unchanged — only the blocking is gone.
  ///
  /// Returns true when the request was QUEUED behind a running pass (the
  /// caller can use this to give the user "will run after the current pass"
  /// feedback instead of a misleading "cleared" toast). False means accepted
  /// for immediate execution or dropped (nothing to clear).
  bool request_clear_glitch_filter();

  bool is_glitch_filter_active();

  void set_signal_invert(const std::vector<bool> &channels);

  /// Synchronous clear of the signal invert. See clear_glitch_filter() for why
  /// the API keeps a blocking variant and the GUI must not use it.
  void clear_signal_invert();

  /// Asynchronous clear for the GUI thread: completes via SignalInvertCleared +
  /// DataUpdated. Returns true when the request was queued behind a running
  /// pass (see request_clear_glitch_filter()).
  bool request_clear_signal_invert();

  bool is_signal_invert_active();

  /// Stop both background tasks. Called from SigSession::Close().
  void stop();

  /// Block (bounded) until no glitch-filter / signal-invert background task
  /// is running. Called by capture/config boundaries (init_signals / capture
  /// start) BEFORE they clear or rebuild the live logic snapshot, because a
  /// running task reads that snapshot by raw pointer (apply/revert) and
  /// would SIGSEGV if it were destroyed underneath it (group3 test_36 crash).
  ///
  /// Returns true if the pool became idle within `max_wait_ms`, false on
  /// timeout. The timeout is honoured: the previous implementation ignored
  /// the parameter and waited forever, so any GUI-reachable caller
  /// (init_signals on a samplerate change / re-capture / device switch)
  /// stalled the event loop for the whole duration of a running task.
  ///
  /// When it returns false the caller MUST NOT tear down the snapshot; the
  /// safe response is to defer/abort the rebuild (the task itself holds
  /// _edit_mutex and will finish on its own).
  bool wait_idle(int max_wait_ms = 15000);

private:
  void glitch_filter_task(const std::map<int, uint32_t> thresholds,
                          const std::map<int, GlitchFilterMode> filter_modes);
  void signal_invert_task(const std::vector<bool> channels);

  /// Submit the undo work onto _filter_pool and hand back its future. An
  /// INVALID future means the request was skipped because there is nothing to
  /// clear. A VALID future means the clear is queued as a pool task: when a
  /// pass is running, the task parks on _edit_mutex and runs the undo after
  /// the pass finishes — the request is never silently dropped any more, and
  /// the sync API/MCP variant now genuinely waits for the data to be restored
  /// even when a pass is in flight (previously an in-flight pass made it
  /// return immediately with the data still filtered).
  /// Both clear paths (sync and async) go through these, so the decision logic
  /// exists once.
  std::future<void> submit_clear_glitch_filter();
  std::future<void> submit_clear_signal_invert();

  /// Worker-side bodies of the two clears. Called with _edit_mutex ALREADY
  /// HELD (by the clear task or by a pass servicing a queued clear), so they
  /// must not take it themselves. They re-check the active flag under that
  /// lock, because the state may have changed between submission and
  /// execution.
  void clear_glitch_filter_locked();
  void clear_signal_invert_locked();

  /// Worker-side task wrappers: take _edit_mutex, honour the latest-writer
  /// intent check (see _edit_intent_seq), then call the *_locked() body.
  void clear_glitch_filter_task(uint64_t intent_seq);
  void clear_signal_invert_task(uint64_t intent_seq);

  /// Rebuild the live snapshot into the target state
  /// "capture data -> signal invert -> glitch filter", starting from
  /// Snapshot::revert_all_edits() (capture-original) instead of copying a
  /// whole backup snapshot back in.
  ///
  /// Returns false when the pass failed (allocation failure or edit-log
  /// budget exhaustion) AND the snapshot was rolled back to capture-original
  /// data. A false return must be treated as "the filter did not take
  /// effect" — the previous code left _glitch_filter_active = true anyway,
  /// which produced the "partially filtered but reported as successful"
  /// state that looks exactly like a stuck filter.
  bool rebuild_filtered_state(const std::map<int, uint32_t> &thresholds,
                              const std::map<int, GlitchFilterMode> &filter_modes);

  /// Bit-invert the requested channels. Iterates SignalModels (the source of
  /// truth for channel metadata) so the boolean-index correspondence matches
  /// the View layer's channel ordering.
  void apply_signal_invert(data::LogicSnapshot *logic,
                           const std::vector<bool> &channels);

  /// Throttled "the sample store has a new visible revision" notice, driven by
  /// the data layer's per-batch batch_callback (see
  /// LogicSnapshot::apply_glitch_filter).
  ///
  /// Why this is needed at all: while a filter pass runs, NOTHING else on the
  /// GUI side invalidates the signal pixmap. Capture has stopped, so no data
  /// packets reach Viewport::feed_in_* and the progress timer is idle; the
  /// decoders are paused; and DataUpdated is broadcast only once, after the
  /// whole pass. The renderer therefore kept blitting its cached pixmap for
  /// the entire pass — which is why the waveform appeared to "refresh only
  /// when scrolled" (a scroll changes scale/offset and forces a rebuild).
  ///
  /// Throttled because one pass can commit thousands of batches (measured:
  /// 3943 batches for a 2.5 GS/s file), and each notice costs the GUI a full
  /// data_updated() pass (layout + margins + scrollbars + pixmap rebuild).
  void notify_batch_committed();

  /// Steady-clock stamp of the last publication. Only the filter worker thread
  /// ever touches it. Zero-value means "never published in this pass", which
  /// makes the first batch of a pass publish immediately.
  std::chrono::steady_clock::time_point _last_batch_refresh{};

  EventBus *_event_bus;
  ISessionState *_state;
  ISessionCoordination *_coord;

  // Gap 1: ThreadPool replaces unique_ptr<std::thread>.
  // ThreadPool manages thread lifetime — no manual join, no self-join risk.
  ThreadPool _filter_pool;

  std::atomic<bool> _glitch_filter_running;
  // Cooperative cancellation for an in-flight pass. Raised by wait_idle()
  // before it starts waiting, so a capture/config boundary does not have to
  // sit through the remainder of a long filter — the scan polls this once per
  // iteration and stops. Cleared afterwards so later passes run normally.
  std::atomic<bool> _filter_cancel{false};
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

  // Latest-writer-wins sequence for edit intents. Every accepted submission
  // (set_glitch_filter, set_signal_invert, submit_clear_*) bumps it; a queued
  // clear task captures its own value at submission and, once it finally gets
  // _edit_mutex, skips its body if the counter has moved on. That preserves
  // the pre-queue semantics of "a clear submitted while a pass runs is
  // superseded by a LATER apply" while still queueing the clear when the
  // clear itself is the latest instruction. Without the check, an apply
  // queued right after a clear would be applied by the pass and then
  // immediately undone by the stale clear.
  std::atomic<uint64_t> _edit_intent_seq{0};

  // Serializes every writer of the live snapshot's derived state: the
  // glitch-filter task, the signal-invert task, and the two clear paths.
  // Before the reversible-edit refactor these paths performed
  // copy_from(live <-> backup); two concurrent copy_from calls raced and
  // SIGSEGV'd (group3 test_36). The same mutual exclusion is still required
  // now that the paths replay an edit log, so the requirement is unchanged.
  //
  // std::timed_mutex (not std::mutex) so a caller that cannot afford an
  // unbounded wait can bound it. Note that no GUI-reachable path takes this
  // lock directly any more: the clear paths submit their work to _filter_pool
  // (request_clear_*) and block HERE, on a worker. That is what removed the
  // multi-second freeze from "undo filter" on a large capture.
  std::timed_mutex _edit_mutex;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_FILTERPROCESSOR_H
