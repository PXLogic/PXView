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

#include "pv/core/scheduler_thread.h"

#include <QMetaObject>
#include <QTimer>

#include <algorithm>

namespace pv {
namespace core {

// Nested definition of SchedulerThread::SchedulerTimerHost (forward-declared
// in the header). Owns the six QTimers on the scheduler thread. All
// start/stop/single-shot requests arrive from the main thread as queued
// functor calls (so the QTimers are always started/stopped on the thread they
// belong to). On timeout, a queued functor is posted back to the owning
// SchedulerThread (main thread). No Q_OBJECT is needed: dispatch uses the
// functor overload of QMetaObject::invokeMethod.
class SchedulerThread::SchedulerTimerHost : public QObject {
public:
  explicit SchedulerTimerHost(SchedulerThread *owner) : _owner(owner) {
    for (int i = 0; i < SchedulerThread::TimerCount; ++i) {
      _timers[i] = new QTimer(this);
      connect(_timers[i], &QTimer::timeout, this,
              [this, i]() { emit_fire(i); });
    }
  }

  void startTimerOnThread(int id, int ms) {
    if (id < 0 || id >= SchedulerThread::TimerCount)
      return;
    _timers[id]->start(ms);
  }
  void stopTimerOnThread(int id) {
    if (id < 0 || id >= SchedulerThread::TimerCount)
      return;
    _timers[id]->stop();
  }
  void singleShotOnThread(int ms) {
    QTimer::singleShot(ms, this, [this]() {
      QMetaObject::invokeMethod(_owner,
                                [owner = _owner]() { owner->on_single_fire(); },
                                Qt::QueuedConnection);
    });
  }

private:
  void emit_fire(int id) {
    // Post back to the main thread (the owner object's thread). Queued
    // functor calls to a destroyed owner are dropped by Qt's event system.
    QMetaObject::invokeMethod(_owner,
                              [owner = _owner, id]() { owner->on_fire(id); },
                              Qt::QueuedConnection);
  }

  SchedulerThread *_owner;
  QTimer *_timers[SchedulerThread::TimerCount] = {};
};

SchedulerThread::SchedulerThread(QObject *parent) : QObject(parent) {
  _active.fill(false);
}

SchedulerThread::~SchedulerThread() { stop(); }

void SchedulerThread::start() {
  if (_running)
    return;
  _host = new SchedulerTimerHost(this);
  _host->moveToThread(&_thread);
  _thread.start();
  _running = true;
}

void SchedulerThread::stop() {
  if (!_running)
    return;
  // Ask the worker to stop every timer, then quit + join.
  for (int i = 0; i < TimerCount; ++i) {
    QMetaObject::invokeMethod(_host, [host = _host, i]() {
                                host->stopTimerOnThread(i);
                              },
                              Qt::QueuedConnection);
  }
  _thread.quit();
  _thread.wait();
  // Clear callbacks: any queued main-thread deliveries still to arrive (from
  // timeouts posted before quit) become no-ops, so a destroyed owner is never
  // called back.
  std::fill(_cbs.begin(), _cbs.end(), nullptr);
  _single_cb = nullptr;
  _active.fill(false);
  delete _host;
  _host = nullptr;
  _running = false;
}

void SchedulerThread::set_callback(TimerId id, std::function<void()> cb) {
  if (id < 0 || id >= TimerCount)
    return;
  _cbs[id] = std::move(cb);
}

void SchedulerThread::start_timer(TimerId id, int ms) {
  if (id < 0 || id >= TimerCount)
    return;
  _active[id] = true;
  _begin[id] = std::chrono::steady_clock::now();
  if (_host) {
    QMetaObject::invokeMethod(_host, [host = _host, id, ms]() {
                                host->startTimerOnThread(id, ms);
                              },
                              Qt::QueuedConnection);
  }
}

void SchedulerThread::stop_timer(TimerId id) {
  if (id < 0 || id >= TimerCount)
    return;
  _active[id] = false;
  if (_host) {
    QMetaObject::invokeMethod(_host, [host = _host, id]() {
                                host->stopTimerOnThread(id);
                              },
                              Qt::QueuedConnection);
  }
}

bool SchedulerThread::is_active(TimerId id) const {
  if (id < 0 || id >= TimerCount)
    return false;
  return _active[id];
}

void SchedulerThread::single_shot(int ms, std::function<void()> cb) {
  _single_cb = std::move(cb);
  if (_host) {
    QMetaObject::invokeMethod(_host, [host = _host, ms]() {
                                host->singleShotOnThread(ms);
                              },
                              Qt::QueuedConnection);
  }
}

long long SchedulerThread::active_time_ms(TimerId id) const {
  if (id < 0 || id >= TimerCount)
    return 0;
  if (!_active[id])
    return 0;
  auto now = std::chrono::steady_clock::now();
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - _begin[id])
          .count());
}

void SchedulerThread::reset_active_time(TimerId id) {
  if (id < 0 || id >= TimerCount)
    return;
  _begin[id] = std::chrono::steady_clock::now();
}

void SchedulerThread::on_fire(int id) {
  // Main thread. Guard against a cleared callback (post-stop delivery).
  if (id < 0 || id >= TimerCount)
    return;
  if (_cbs[id])
    _cbs[id]();
}

void SchedulerThread::on_single_fire() {
  if (_single_cb) {
    auto cb = std::move(_single_cb);
    _single_cb = nullptr;
    cb();
  }
}

} // namespace core
} // namespace pv
