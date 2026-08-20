/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef PXVIEW_CORE_SCHEDULER_THREAD_H
#define PXVIEW_CORE_SCHEDULER_THREAD_H

#include <QObject>
#include <QThread>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>

namespace pv {
namespace core {

/**
 * SchedulerThread — runs the capture-cadence QTimers on a dedicated thread so
 * GUI/paint stalls never skew the cadence (P6 of the performance/thread-model
 * work). Timeouts fire on the scheduler thread and the registered callback is
 * marshaled back to the MAIN thread via a queued QMetaObject::invokeMethod;
 * callbacks therefore never run on the scheduler thread and always run on the
 * main thread.
 *
 * Usage (all public methods are main-thread-only):
 *   SchedulerThread sched;
 *   sched.set_callback(SchedulerThread::FeedTimer, [] { ... });  // runs on main
 *   sched.start();
 *   sched.start_timer(SchedulerThread::FeedTimer, 50);
 *   ...
 *   sched.stop();   // joins the worker thread; idempotent
 *
 * The worker thread + its QTimers are created lazily by start(). The
 * callbacks capture whatever context they need; on stop()/destruction all
 * callbacks are cleared and queued deliveries become no-ops, so a destroyed
 * owner (e.g. CaptureManager) can never be called back.
 *
 * Neither class needs Q_OBJECT: all cross-thread dispatch uses the functor
 * overload of QMetaObject::invokeMethod (which only requires a QObject
 * context), so no moc is involved.
 */
class SchedulerThread : public QObject {
public:
  // The capture cadence timers (mirrors CaptureManager's six DsTimers).
  enum TimerId {
    FeedTimer = 0,
    OutTimer,
    RepeatTimer,
    RepeatWaitProgTimer,
    RefreshRtTimer,
    TrigCheckTimer,
    TimerCount
  };

  explicit SchedulerThread(QObject *parent = nullptr);
  ~SchedulerThread() override;

  // Start the worker thread + timers (idempotent). Must be called on the
  // main thread.
  void start();
  // Stop all timers, quit + join the worker thread, clear callbacks.
  // Main-thread only; idempotent.
  void stop();

  // Register the timeout callback for a timer. The callback runs on the
  // MAIN thread. Replaces any previously registered callback for the id.
  void set_callback(TimerId id, std::function<void()> cb);

  // Start/stop a periodic timer (fires on the worker thread, callback runs
  // on the main thread). start_timer() is idempotent per timer (restarts).
  void start_timer(TimerId id, int ms);
  void stop_timer(TimerId id);
  bool is_active(TimerId id) const;

  // Fire `cb` once after `ms` on the main thread (worker-side single-shot).
  void single_shot(int ms, std::function<void()> cb);

  // Active-time bookkeeping (main-thread only; matches DsTimer semantics).
  long long active_time_ms(TimerId id) const;
  void reset_active_time(TimerId id);

private:
  // Runs on the main thread (queued from the worker TimerHost).
  void on_fire(int id);
  void on_single_fire();

  class SchedulerTimerHost; // defined in scheduler_thread.cpp (worker thread)
  SchedulerTimerHost *_host = nullptr; // owned by this; lives on _thread

  std::array<std::function<void()>, TimerCount> _cbs;
  std::function<void()> _single_cb;

  // Main-thread bookkeeping (mimics DsTimer::GetActiveTimeLong).
  std::array<bool, TimerCount> _active{};
  std::array<std::chrono::steady_clock::time_point, TimerCount> _begin;

  QThread _thread;
  bool _running = false;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_SCHEDULER_THREAD_H
