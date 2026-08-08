#include "decodetaskmanager.h"

#include "documentregistry.h"
#include "eventbus.h"
#include "sessionstatecontext.h"
#include "../sigsession.h"  // SessionData full definition
#include "../data/decoderstack.h"
#include "../data/sessiondocument.h"
#include "../data/signalmodel.h"
#include "../data/logicsnapshot.h"
#include "../interface/events.h"
#include "../log.h"

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
  // P0-2 fix: request stop on all running tasks BEFORE joining, so decode
  // threads actually exit their work loops. Without this, join() would wait
  // indefinitely for a decoder that's still processing data.
  {
    std::lock_guard<std::mutex> lock(_running_tasks_mutex);
    for (auto &stack : _running_tasks) {
      if (stack)
        stack->stop_decode_work();
    }
  }

  // P0-1A fix: "lock-swap-unlock-join" pattern. We must NOT hold
  // _running_tasks_mutex while calling join(), because the worker threads
  // need to acquire it in decode_single_task() to finish their cleanup.
  // Holding the lock during join() creates a classic deadlock:
  //   main thread: holds lock → join → waits for worker
  //   worker thread: waits for lock → can never finish
  // The fix: atomically swap the thread vector out under the lock, then
  // release the lock and join outside the critical section.
  std::vector<std::thread> threads_to_join;
  {
    std::lock_guard<std::mutex> lock(_running_tasks_mutex);
    threads_to_join.swap(_decode_threads);
  }
  // Join outside the lock — workers can now acquire it to finish cleanup.
  for (auto &t : threads_to_join) {
    if (t.joinable())
      t.join();
  }

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
  for (auto m : _state->signal_models()) {
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

  // L-2 fix: create the thread object BEFORE entering the critical section.
  // Creating a std::thread starts the OS thread immediately, and the new
  // thread will try to acquire _running_tasks_mutex in decode_single_task().
  // If we create it while holding the lock, the new thread blocks until we
  // release — unnecessary contention, especially when batch-starting N
  // decoders. By creating the thread first and then moving it into the
  // vector under the lock, we avoid this.
  std::thread new_thread(
      &DecodeTaskManager::decode_single_task, this, stack);

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
        // Thread was already started — add it to _decode_threads so that
        // stop()/clear_all_decode_task() will join it. Previously this used
        // detach(), which left the thread accessing `this` (DecodeTaskManager*)
        // after potential destruction — a use-after-free risk. The thread
        // will quickly exit because begin_decode_work() returns immediately
        // when _decode_state != Stopped, and decode_single_task() will find
        // the task absent from _running_tasks.
        _decode_threads.push_back(std::move(new_thread));
        return;
      }
    }
    _running_tasks.push_back(stack);
    _decode_threads.push_back(std::move(new_thread));
  }
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

  // Phase 2: P0-1A fix — swap threads out under lock, then join WITHOUT
  // holding the lock. Worker threads need the lock to finish cleanup.
  std::vector<std::thread> threads_to_join;
  {
    std::lock_guard<std::mutex> lock(_running_tasks_mutex);
    threads_to_join.swap(_decode_threads);
  }
  for (auto &t : threads_to_join) {
    if (t.joinable())
      t.join();
  }

  // Phase 3: clear _running_tasks after all threads are joined.
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

  if (!task->_delete_flag) {
    task->begin_decode_work();
  }

  if (task->_delete_flag) {
    pxv_info("destroy a decoder in task thread");

    // L-1 fix: the previous std::atomic_thread_fence was a no-op (no paired
    // atomic load/store). Instead of a fence + direct call (which has a
    // TOCTOU window on bClose()), we post signals_changed() to the main
    // thread via post_async_dispatch. The main thread will execute it after
    // any pending close operations, so the session state is consistent.
    pv::core::EventBus::post_async_dispatch([coord = _coord]() {
      if (coord && !coord->bClose()) {
        coord->signals_changed();
      }
    });
  }

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

  // Track C2: [PWMDBG] debug log removed

  for (auto stack : _state->decode_traces()) {
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
