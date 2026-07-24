#ifndef PXVIEW_CORE_DECODETASKMANAGER_H
#define PXVIEW_CORE_DECODETASKMANAGER_H

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../data/sessiondata.h"

namespace pv {

namespace data {
class DecoderStack;
class SessionDocument;
} // namespace data

namespace core {

class EventBus;
class SessionStateContext;

/**
 * DecodeTaskManager — owns the decode thread pool (_decode_threads) and the
 * list of in-flight decode tasks (_running_tasks). Extracted from SigSession
 * (SubTask 10.3) as a mechanical refactoring: no behavior change.
 *
 * Holds an injected EventBus* (for broadcast<T>) and a SessionStateContext*
 * (for accessing signal_models / view_data / document_registry /
 * decode_traces() / signals_changed() etc.). modernize-core-layer-radical
 * phase 1 replaced the previous SigSession* + friend-declaration coupling.
 */
class DecodeTaskManager {
public:
  DecodeTaskManager(EventBus *bus, SessionStateContext *state);
  ~DecodeTaskManager();

  void add_decode_task(std::shared_ptr<data::DecoderStack> stack);
  void decode_single_task(std::shared_ptr<data::DecoderStack> task);
  void remove_decode_task(std::shared_ptr<data::DecoderStack> stack);
  void clear_all_decode_task(int &runningDex);
  void clear_all_decode_task2();
  void start_all_decode_tasks();
  void attach_data_to_signal(SessionData *data);
  void rst_decoder(int index, data::SessionDocument *doc = nullptr);
  void rst_decoder_by_key_handel(void *handel,
                                 data::SessionDocument *doc = nullptr);

  /// Used by feed_in_logic to check if any decode task is in flight.
  /// Preserves the existing lock-free read behavior.
  bool has_running_tasks() const { return !_running_tasks.empty(); }

  /// Used by remove_decoder to check if a specific stack is still being
  /// processed by a decode thread. Locks the mutex for a consistent read.
  bool is_task_running(std::shared_ptr<data::DecoderStack> stack);

  /// Stop all decode threads. Called from SigSession::Close().
  void stop();

private:
  EventBus *_event_bus;
  // Shared session state (signal_models / view_data / document_registry /
  // bClose / decode_traces() / get_decoder_trace() /
  // get_trace_index_by_key_handel() / data_updated() / signals_changed())
  // accessed via SessionStateContext accessors. modernize-core-layer-radical
  // phase 1 replaced the previous SigSession* + friend-declaration coupling.
  SessionStateContext *_state;

  mutable std::mutex _running_tasks_mutex;
  std::vector<std::thread> _decode_threads;
  std::vector<std::shared_ptr<data::DecoderStack>> _running_tasks;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_DECODETASKMANAGER_H
