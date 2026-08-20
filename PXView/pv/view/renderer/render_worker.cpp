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

#include "pv/view/renderer/render_worker.h"

#include <QPainter>

namespace pv {
namespace view {

RenderWorker::RenderWorker() = default;

RenderWorker::~RenderWorker() { stop(); }

void RenderWorker::start() {
  if (_running)
    return;
  _running = true;
  _stop = false;
  _thread = std::thread([this]() { run(); });
}

void RenderWorker::stop() {
  if (!_running)
    return;
  {
    std::unique_lock<std::mutex> lock(_mutex);
    _stop = true;
  }
  _cv.notify_all();
  if (_thread.joinable())
    _thread.join();
  {
    std::unique_lock<std::mutex> lock(_mutex);
    for (auto &f : _frames) {
      f.image = QImage();
      f.seq = 0;
      f.acquired = false;
      f.ops.clear();
    }
    _has_pending = false;
    _pending = Job{};
    _next_seq = 1;
  }
  _running = false;
}

void RenderWorker::submit(Job job) {
  {
    std::unique_lock<std::mutex> lock(_mutex);
    if (!_running)
      return;
    _pending = std::move(job);
    _has_pending = true;
  }
  _cv.notify_one();
}

bool RenderWorker::try_acquire(const QSize &size, qreal dpr,
                               const QImage **out) {
  std::unique_lock<std::mutex> lock(_mutex);
  Frame *best = nullptr;
  for (auto &f : _frames) {
    if (f.seq == 0 || f.acquired)
      continue;
    if (f.size != size || !qFuzzyCompare(f.dpr, dpr))
      continue;
    if (!best || f.seq > best->seq)
      best = &f;
  }
  if (!best)
    return false;
  best->acquired = true;
  *out = &best->image;
  return true;
}

bool RenderWorker::has_frame(const QSize &size, qreal dpr) {
  std::unique_lock<std::mutex> lock(_mutex);
  for (auto &f : _frames) {
    if (f.seq != 0 && !f.acquired && f.size == size &&
        qFuzzyCompare(f.dpr, dpr))
      return true;
  }
  return false;
}

void RenderWorker::release() {
  std::unique_lock<std::mutex> lock(_mutex);
  for (auto &f : _frames) {
    if (f.acquired) {
      // Keep the published seq: the completed frame stays blittable for later
      // paints. The worker recycles the oldest frame when it needs a slot.
      f.acquired = false;
      return;
    }
  }
}

void RenderWorker::set_published_callback(std::function<void()> cb) {
  std::unique_lock<std::mutex> lock(_mutex);
  _published_cb = std::move(cb);
}

void RenderWorker::run() {
  for (;;) {
    Job job;
    Frame *slot = nullptr;
    {
      std::unique_lock<std::mutex> lock(_mutex);
      _cv.wait(lock, [this]() { return _stop || _has_pending; });
      if (_stop)
        return;
      job = std::move(_pending);
      _has_pending = false;
      // Prefer a free slot (not published, not being blitted); otherwise
      // recycle the oldest published non-acquired frame (triple-buffer
      // cycling). At most one frame is acquired at a time (GUI thread only),
      // so a slot is always found.
      for (auto &f : _frames) {
        if (f.seq == 0 && !f.acquired) {
          slot = &f;
          break;
        }
      }
      if (!slot) {
        Frame *oldest = nullptr;
        for (auto &f : _frames) {
          if (f.acquired)
            continue;
          if (!oldest || f.seq < oldest->seq)
            oldest = &f;
        }
        slot = oldest;
      }
      if (!slot)
        continue; // all slots acquired (should not happen): drop, next submit refreshes
      slot->size = job.size;
      slot->dpr = job.dpr;
      slot->vOffset = job.vOffset;
      slot->ops = std::move(job.ops);
    }

    // Render into the slot's QImage (exclusive to this thread: acquired==false
    // and the GUI never draws into a non-acquired slot).
    if (slot->image.size() != slot->size ||
        !qFuzzyCompare(slot->image.devicePixelRatioF(), slot->dpr)) {
      slot->image = QImage(slot->size, QImage::Format_ARGB32_Premultiplied);
      slot->image.setDevicePixelRatio(slot->dpr);
    }
    slot->image.fill(Qt::transparent);
    {
      QPainter qp(&slot->image);
      qp.translate(0, -slot->vOffset);
      for (auto &op : slot->ops)
        op(qp);
    }
    slot->ops.clear();

    // Publish with a monotonic sequence number.
    std::function<void()> cb;
    {
      std::unique_lock<std::mutex> lock(_mutex);
      slot->seq = _next_seq++;
      cb = _published_cb;
    }
    if (cb)
      cb();
  }
}

} // namespace view
} // namespace pv
