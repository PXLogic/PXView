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
 * Implementation: 一个 mutex + 一个 condition_variable + 两个 atomic<bool>
 * （_ready / _broken）。生产者置位后用 notify_all() 唤醒；消费者在 wait() 里
 * 以 _ready 为谓词做 cv.wait / cv.wait_for。这样得到的是"线程安全的同步等待"，
 * 完全不依赖任何事件队列（Qt 事件循环、GLib main loop 等）——因此在主线程被
 * 模态循环占住、或工作线程还没有回到事件循环时，等待方照样能被立刻唤醒。
 *
 * ── 接口语义 ──
 *
 *   set_result()               标记操作成功完成，唤醒所有等待者
 *   set_broken()               标记操作失败，is_broken() 之后返回 true，
 *                              让等待方能区分"成功"与"失败"
 *   wait(timeout_ms = 0)       阻塞到完成或超时；0 = 无限等待
 *   is_ready() / is_broken()   无锁查询（atomic 读）
 *   reset()                    复用前清零（只能由持有者在开新操作前调用）
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
