#ifndef PXVIEW_CORE_SHARED_STATE_H
#define PXVIEW_CORE_SHARED_STATE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace pv {
namespace core {

/**
 * SharedState — reusable promise/future shared state primitive.
 *
 * Modeled after Logic2's Saleae::Tasks::Detail::SharedState
 * (graph_dll/core/task_executor/include/task_executor.h), which uses
 * mutex + condition_variable + atomic flags to provide thread-safe
 * synchronous waits WITHOUT depending on any event queue (Qt, GLib,
 * etc.).
 *
 * ── Logic2 architecture comparison ──
 *
 *   Logic2 SharedState        |  PXView SharedState
 *   --------------------------+--------------------------
 *   SetResult()               |  set_result()
 *   SetBroken()               |  set_broken()
 *   WaitOnState()             |  wait(uint64_t timeout_ms = 0)
 *   ready (atomic<bool>)      |  _ready (atomic<bool>)
 *   broken (atomic<bool>)     |  _broken (atomic<bool>)
 *
 * Logic2's SharedState is the internal state of a std::promise/future
 * pair, used by TaskExecutor::Schedule and Mailbox::SendSync. PXView
 * uses it as a standalone primitive for any "wait for async operation
 * to complete" pattern, completely bypassing the Qt event queue.
 *
 * ── Usage pattern ──
 *
 *   // Producer (worker thread):
 *   _shared_state.set_result();   // or set_broken() on error
 *
 *   // Consumer (main thread):
 *   bool ok = _shared_state.wait(5000);  // 5s timeout
 *   if (ok && !_shared_state.is_broken()) {
 *       // success
 *   }
 *
 * ── Thread safety ──
 *
 * All public methods are thread-safe. Multiple consumers can call
 * wait() concurrently; set_result()/set_broken() wakes all of them.
 *
 * ── Lifecycle ──
 *
 * Call reset() before reusing a SharedState instance for a new
 * operation. reset() is NOT thread-safe with concurrent wait() —
 * it should only be called by the owning thread before starting a
 * new operation.
 */
class SharedState {
public:
  SharedState() = default;
  ~SharedState() = default;

  SharedState(const SharedState &) = delete;
  SharedState &operator=(const SharedState &) = delete;

  /// Producer: mark the operation as successfully completed.
  /// Wakes all threads blocked in wait().
  void set_result() {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _ready.store(true, std::memory_order_release);
    }
    _cv.notify_all();
  }

  /// Producer: mark the operation as failed/broken.
  /// Wakes all threads blocked in wait(). After this, is_broken()
  /// returns true so consumers can distinguish failure from success.
  void set_broken() {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _broken.store(true, std::memory_order_release);
      _ready.store(true, std::memory_order_release);
    }
    _cv.notify_all();
  }

  /// Consumer: block until the operation completes (set_result or
  /// set_broken) or timeout expires.
  ///
  /// @param timeout_ms  Maximum wait time in milliseconds.
  ///                    0 means infinite wait (no timeout).
  /// @return true if the operation completed (ready), false on timeout.
  bool wait(uint64_t timeout_ms = 0) {
    std::unique_lock<std::mutex> lock(_mutex);
    if (timeout_ms == 0) {
      _cv.wait(lock, [this]() {
        return _ready.load(std::memory_order_acquire);
      });
      return true;
    }
    return _cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this]() {
                          return _ready.load(std::memory_order_acquire);
                        });
  }

  /// Check if the operation has completed (success or broken).
  bool is_ready() const { return _ready.load(std::memory_order_acquire); }

  /// Check if the operation was broken (failed).
  bool is_broken() const { return _broken.load(std::memory_order_acquire); }

  /// Reset the state for reuse. NOT thread-safe with concurrent wait()
  /// — call only from the owning thread before starting a new operation.
  void reset() {
    std::lock_guard<std::mutex> lock(_mutex);
    _ready.store(false, std::memory_order_release);
    _broken.store(false, std::memory_order_release);
  }

private:
  std::mutex _mutex;
  std::condition_variable _cv;
  std::atomic<bool> _ready{false};
  std::atomic<bool> _broken{false};
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_SHARED_STATE_H
