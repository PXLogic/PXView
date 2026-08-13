#ifndef PXVIEW_CORE_THREAD_POOL_H
#define PXVIEW_CORE_THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace pv {
namespace core {

/**
 * ThreadPool — lightweight worker thread pool with wait_for_idle support.
 *
 * Plan B Phase 3: provides a unified thread management layer for
 * DecodeTaskManager, FilterProcessor, and StoreSession, replacing the
 * scattered std::thread instances (~27 sites across 6 modules).
 *
 * Design goals:
 *   - Simple: no task priorities, no cancellation, no task handles.
 *     Just submit() + wait_for_idle(), matching the current usage pattern.
 *   - Safe: destructor signals stop + joins all threads, so no detached
 *     threads survive after the pool is destroyed.
 *   - Non-blocking submit: submit() never blocks (queue is unbounded).
 *
 * Not modeled after Logic2's TaskExecutor (which has full task lifecycle
 * management with TaskHandle/Cancel/Status). PXView doesn't need that
 * complexity — the current usage is fire-and-forget with join-on-stop.
 */
class ThreadPool {
public:
  explicit ThreadPool(size_t thread_count = 2)
      : _stop_flag(false) {
    for (size_t i = 0; i < thread_count; ++i) {
      _workers.emplace_back([this]() { worker_loop(); });
    }
  }

  ~ThreadPool() { shutdown(); }

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  /// Submit a task. Returns a future so the caller can wait if needed.
  /// submit() is thread-safe.
  std::future<void> submit(std::function<void()> task) {
    auto promise = std::make_shared<std::promise<void>>();
    auto fut = promise->get_future();
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _tasks.push([promise, task = std::move(task)]() {
        task();
        promise->set_value();
      });
    }
    _cv.notify_one();
    return fut;
  }

  /// Block until all queued tasks are completed and no worker is active.
  /// Thread-safe. Can be called concurrently with submit() (it will
  /// wait for tasks submitted before the call).
  void wait_for_idle() {
    std::unique_lock<std::mutex> lock(_mutex);
    _idle_cv.wait(lock, [this]() {
      return _tasks.empty() && _active_count.load() == 0;
    });
  }

  /// Grow the pool to at least min_threads worker threads.
  /// If the pool already has >= min_threads workers, this is a no-op.
  /// Safe to call concurrently with submit(). New threads start
  /// immediately and can pick up queued tasks.
  /// This restores the pre-Plan-B parallelism where each decoder ran
  /// on its own dedicated thread.
  void grow(size_t min_threads) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_stop_flag.load())
      return;
    while (_workers.size() < min_threads) {
      _workers.emplace_back([this]() { worker_loop(); });
    }
  }

  /// Return the current number of worker threads.
  size_t worker_count() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _workers.size();
  }

  /// Signal all workers to stop and join them. Called by destructor.
  /// Can be called manually (e.g. from SigSession::Close()).
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (_stop_flag.load())
        return;
      _stop_flag.store(true);
    }
    _cv.notify_all();
    for (auto &w : _workers) {
      if (w.joinable())
        w.join();
    }
    _workers.clear();
  }

private:
  void worker_loop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [this]() {
          return _stop_flag.load() || !_tasks.empty();
        });
        if (_stop_flag.load() && _tasks.empty())
          return;
        task = std::move(_tasks.front());
        _tasks.pop();
        _active_count.fetch_add(1);
      }
      task();
      _active_count.fetch_sub(1);
      {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_tasks.empty() && _active_count.load() == 0)
          _idle_cv.notify_all();
      }
    }
  }

  std::vector<std::thread> _workers;
  std::queue<std::function<void()>> _tasks;
  mutable std::mutex _mutex;
  std::condition_variable _cv;
  std::condition_variable _idle_cv;
  std::atomic<bool> _stop_flag;
  std::atomic<int> _active_count{0};
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_THREAD_POOL_H
