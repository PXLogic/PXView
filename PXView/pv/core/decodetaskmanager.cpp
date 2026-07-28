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
#include <chrono>
#include <memory>

namespace pv {
namespace core {

DecodeTaskManager::DecodeTaskManager(EventBus *bus, SessionStateContext *state)
    : _event_bus(bus), _state(state) {}

DecodeTaskManager::~DecodeTaskManager() { stop(); }

void DecodeTaskManager::stop() {
  // Join all decode threads before destruction. The threads are owned by
  // this manager. Called from SigSession::Close() after clear_all_documents_
  // decoders() has already requested stop_decode_work() on every stack.
  for (auto &t : _decode_threads) {
    if (t.joinable())
      t.join();
  }
  _decode_threads.clear();

  std::lock_guard<std::mutex> lock(_running_tasks_mutex);
  _running_tasks.clear();
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
      m->set_snapshot(data->get_logic());
      break;
    case SR_CHANNEL_ANALOG:
      m->set_snapshot(data->get_analog());
      break;
    case SR_CHANNEL_DSO:
      m->set_snapshot(data->get_dso());
      break;
    }
  }

  // R1: snapshot pointers changed, so notify listeners that the data view
  // and the signal list need refreshing. Centralizing the notification here
  // removes the need for callers to manually re-fire these callbacks.
  _state->data_updated();
  _state->signals_changed();

  // [PWMDBG] confirm WHICH data buffer the models were attached to and how
  // many samples it currently holds (race: decode before buffer swap?)
  if (data->get_logic()) {
    pxv_info("[PWMDBG] attach_data_to_signal: data=%p, logic=%p, sample_count=%llu, ring=%llu",
             (void *)data, (void *)data->get_logic(),
             (unsigned long long)data->get_logic()->get_sample_count(),
             (unsigned long long)data->get_logic()->get_ring_sample_count());
  } else {
    pxv_info("[PWMDBG] attach_data_to_signal: data=%p, logic=NULL", (void *)data);
  }
}

void DecodeTaskManager::add_decode_task(
    std::shared_ptr<data::DecoderStack> stack) {
  // Ensure SignalModels have valid snapshot pointers before the decode thread
  // starts. SignalModels may have been recreated with NULL snapshots (e.g. by
  // reload() during TabContext::activate()). Without this, decoders fail with
  // "没有设置需要解码哪些通道的数据". This matches the pattern in
  // start_all_decode_tasks() and rst_decoder().
  attach_data_to_signal(_state->view_data());

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

  _decode_threads.push_back(
      std::thread(&DecodeTaskManager::decode_single_task, this, stack));
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

  for (auto &t : _decode_threads) {
    if (t.joinable())
      t.join();
  }
  _decode_threads.clear();

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

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!_state->bClose()) {
      _state->signals_changed();
    }
  }

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
  // SignalModels may have been recreated (e.g. by reload() during
  // TabContext::activate()) with NULL snapshot pointers. Re-attach _view_data
  // so do_decode_work() can find a valid snapshot via SignalModel::snapshot().
  // Without this, decoders fail with "没有设置需要解码哪些通道的数据".
  // Note: add_decode_task() also calls attach_data_to_signal internally for
  // single-stack callers. We call it here once before the loop for efficiency
  // (single attach instead of N attaches).
  attach_data_to_signal(_state->view_data());

  pxv_info("[PWMDBG] start_all_decode_tasks: stacks=%zu, view_data=%p",
           _state->decode_traces().size(), (void *)_state->view_data());

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

  pxv_info("[PWMDBG] rst_decoder: index=%d, stack=%p", index,
           stack ? stack.get() : nullptr);

  if (stack) {
    remove_decode_task(stack); // remove old task
    stack->clear();
    // SignalModels may have been recreated by reload() (e.g. during
    // TabContext::activate()) with NULL snapshot pointers. The
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
