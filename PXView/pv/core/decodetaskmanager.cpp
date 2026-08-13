#include "pv/core/decodetaskmanager.h"

#include "pv/core/documentregistry.h"
#include "pv/core/eventbus.h"
#include "pv/core/sessionstatecontext.h"
#include "pv/session/sigsession.h"  // SessionData full definition
#include "pv/data/stack/decoderstack.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/interface/events.h"
#include "pv/base/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

namespace pv {
namespace core {

DecodeTaskManager::DecodeTaskManager(EventBus *bus, ISessionState *state, ISessionCoordination *coord)
    : _event_bus(bus), _state(state), _coord(coord) {}

DecodeTaskManager::~DecodeTaskManager() { stop(); }

void DecodeTaskManager::stop() {
  // P0-2 fix: request stop on all running tasks BEFORE shutting down the
  // pool, so decode threads actually exit their work loops.
  {
    std::lock_guard<std::mutex> lock(_running_tasks_mutex);
    for (auto &stack : _running_tasks) {
      if (stack)
        stack->stop_decode_work();
    }
  }

  // Plan B Phase 3: ThreadPool::shutdown() joins all worker threads.
  // No need for the old lock-swap-unlock-join pattern — ThreadPool
  // handles it internally.
  _decode_pool.shutdown();

  // After all threads are joined, clear _running_tasks.
  {
    std::lock_guard<std::mutex> lock(_running_tasks_mutex);
    _running_tasks.clear();
  }
}

void DecodeTaskManager::attach_data_to_signal(SessionData *data) {
  if (!data)
    return;

  // Update each SignalModel's snapshot pointer so consumers of SignalModel
  // can access the most recent snapshot data. The void* type is resolved
  // based on SignalModel::type().
  for (auto m : _state->signal_models_snapshot()) {
    switch (m->type()) {
    case SR_CHANNEL_LOGIC:
      m->set_snapshot(data->logic_shared());
      break;
    case SR_CHANNEL_ANALOG:
      m->set_snapshot(data->analog_shared());
      break;
    case SR_CHANNEL_DSO:
      m->set_snapshot(data->dso_shared());
      break;
    }
  }

  // R1: snapshot pointers changed, so notify listeners that the data view
  // and the signal list need refreshing. Centralizing the notification here
  // removes the need for callers to manually re-fire these callbacks.
  _coord->data_updated();
  _coord->signals_changed();

  // [PWMDBG] debug logs removed (Track C2)
  // SignalModels may have been recreated with nullptr snapshots — attach_data_to_signal
  // ensures valid snapshot pointers before decode threads start.
}

void DecodeTaskManager::add_decode_task(
    std::shared_ptr<data::DecoderStack> stack) {
  // Ensure SignalModels have valid snapshot pointers before the decode thread
  // starts. SignalModels may have been recreated with nullptr snapshots (e.g. by
  // reload() during TabContext::activate()). Without this, decoders fail with
  // "没有设置需要解码哪些通道的数据". This matches the pattern in
  // start_all_decode_tasks() and rst_decoder().
  attach_data_to_signal(_state->view_data());

  // Plan B Phase 3: use ThreadPool instead of raw std::thread.
  // ThreadPool manages thread lifetime — no manual join needed.
  {
    std::lock_guard<std::mutex> lock(_running_tasks_mutex);
    // 防止重复添加:RevEndPacket 和 CopyToDocDone 都会调用
    // start_all_decode_tasks(),若同一 stack 已在解码中,跳过避免
    // begin_decode_work() 的 _decode_state == Stopped 断言失败。
    for (const auto &s : _running_tasks) {
      if (s == stack) {
        pxv_info("DecodeTaskManager::add_decode_task: stack %p already "
                 "running, skip duplicate",
                 stack.get());
        return;
      }
    }
    _running_tasks.push_back(stack);
  }

  // Submit to thread pool — task runs on a pool worker thread.
  // Capture stack by value (shared_ptr copy) to keep it alive.
  auto self = this;
  _decode_pool.submit([self, stack]() {
    self->decode_single_task(stack);
  });
}

void DecodeTaskManager::remove_decode_task(
    std::shared_ptr<data::DecoderStack> stack) {
  stack->stop_decode_work();
}

bool DecodeTaskManager::is_task_running(
    std::shared_ptr<data::DecoderStack> stack) {
  std::lock_guard<std::mutex> lock(_running_tasks_mutex);
  for (auto task : _running_tasks) {
    if (task == stack)
      return true;
  }
  return false;
}

void DecodeTaskManager::clear_all_decode_task(int &runningDex) {
  // Phase 1: request all decoders to stop (under lock).
  {
    std::lock_guard<std::mutex> lock(_running_tasks_mutex);
    for (auto stack : _running_tasks) {
      if (stack)
        stack->stop_decode_work();
    }
  }

  runningDex = -1;
  for (auto doc : _state->document_registry()->get_all_documents()) {
    int dex = 0;
    for (auto stack : doc->get_decoder_stacks()) {
      if (stack->IsRunning()) {
        stack->stop_decode_work();
        if (doc == _state->document_registry()->get_active_document())
          runningDex = dex;
      }
      dex++;
    }
  }

  // Plan B Phase 3: ThreadPool::wait_for_idle() replaces the old
  // lock-swap-unlock-join pattern. Wait for all decode tasks to finish.
  _decode_pool.wait_for_idle();

  // Clear _running_tasks after all tasks are done.
  {
    std::lock_guard<std::mutex> lock(_running_tasks_mutex);
    _running_tasks.clear();
  }
}

void DecodeTaskManager::clear_all_decode_task2() {
  int run_dex = 0;
  clear_all_decode_task(run_dex);
}

void DecodeTaskManager::decode_single_task(
    std::shared_ptr<data::DecoderStack> task) {
  pxv_info("------->decode thread start");

  // P0-3 fix: _delete_flag is removed. The task's lifetime is managed by
  // shared_ptr. Simply call begin_decode_work() — if the task was stopped
  // via stop_decode_work(), begin_decode_work() returns immediately because
  // _decode_state != Stopped.
  task->begin_decode_work();

  // P0-1B fix: do NOT detach/erase ourselves from _decode_threads. The
  // previous code detached the current thread and erased it from the
  // vector, which meant stop()/clear_all_decode_task() could never join it.
  // The detached thread would then continue accessing _state/_event_bus
  // after they were destroyed. Instead, we only remove the task from
  // _running_tasks and let the thread be joined by the stop() path.
  {
    std::lock_guard<std::mutex> lock(_running_tasks_mutex);
    auto it = std::find(_running_tasks.begin(), _running_tasks.end(), task);
    if (it != _running_tasks.end())
      _running_tasks.erase(it);

    if (_running_tasks.empty()) {
      // Check if _view_data and its logic snapshot are valid before calling
      // decode_end
      if (_state->view_data() != nullptr &&
          _state->view_data()->get_logic() != nullptr) {
        _state->view_data()->get_logic()->decode_end();
      }
      // Phase 3 fix: signal the SharedState DIRECTLY on the decode thread,
      // BEFORE broadcasting DecodeDone. This allows wait_for_decode_complete()
      // (main thread) to be woken via cv.wait_for without depending on the
      // Qt event queue — the same pattern used by DeviceSessionStopped() for
      // capture-complete. Without this, the main thread would be blocked in
      // SharedState::wait(), unable to pump the event queue to receive the
      // DecodeDone event, creating a circular dependency (deadlock).
      _state->notify_decode_complete();
      // B1.2: emit the typed DecodeDone event so IEventListener consumers
      // (e.g. a future headless decode-done handler) can react without going
      // through the legacy ISessionStateCallback::decode_done path (which is
      // currently invoked only from DecodeTrace::on_decode_done in the View
      // layer).
      _event_bus->broadcast_async<interface::DecodeDone>({});
    }
  }

  pxv_info("------->decode thread end");
}

void DecodeTaskManager::start_all_decode_tasks() {
  // Track C2: [PWMDBG] debug log removed
  // SignalModels may have been recreated (e.g. by reload() during
  // TabContext::activate()) with nullptr snapshot pointers. Re-attach _view_data
  // so do_decode_work() can find a valid snapshot via SignalModel::snapshot().
  // Without this, decoders fail with "没有设置需要解码哪些通道的数据".
  // Note: add_decode_task() also calls attach_data_to_signal internally for
  // single-stack callers. We call it here once before the loop for efficiency
  // (single attach instead of N attaches).
  attach_data_to_signal(_state->view_data());

  // Grow the thread pool to match the number of decode tasks so that
  // each decoder can run on its own thread in parallel, restoring the
  // pre-Plan-B behavior. The original code created one std::thread per
  // decoder; commit c454899a replaced that with a fixed 2-thread pool,
  // which serialized decoding. grow() adds worker threads as needed.
  auto traces = _state->decode_traces();
  if (!traces.empty()) {
    _decode_pool.grow(traces.size());
  }

  for (auto stack : traces) {
    stack->set_capture_end_flag(true);
    stack->frame_ended();
    add_decode_task(stack);
  }
}

void DecodeTaskManager::rst_decoder(int index, data::SessionDocument *doc) {
  data::SessionDocument *target = doc ? doc : _state->document_registry()->get_active_document();
  // The decoder options dialog (DecodeTrace::create_popup(false)) is now
  // shown by the View layer (View::rst_decoder_by_key_handel) BEFORE this
  // function is called. If the user cancels the dialog, View does not
  // forward to Core at all, so this reset path only runs when the user has
  // already accepted new settings. Core then just clears the existing
  // decode task and re-adds it.
  auto stack = _state->get_decoder_trace(index, target);

  // Track C2: [PWMDBG] debug log removed

  if (stack) {
    remove_decode_task(stack); // remove old task
    stack->clear();
    // SignalModels may have been recreated by reload() (e.g. during
    // TabContext::activate()) with nullptr snapshot pointers. The
    // add_decode_task() call below will ensure data attachment internally,
    // so do_decode_work() can find a valid snapshot via
    // SignalModel::snapshot().
    add_decode_task(stack);
  }
}

void DecodeTaskManager::rst_decoder_by_key_handel(void *handel,
                                                  data::SessionDocument *doc) {
  data::SessionDocument *target = doc ? doc : _state->document_registry()->get_active_document();
  int dex = _state->get_trace_index_by_key_handel(handel, target);
  rst_decoder(dex, target);
}

} // namespace core
} // namespace pv
