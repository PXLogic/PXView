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

#ifndef PXVIEW_PV_VIEW_RENDERER_RENDER_WORKER_H
#define PXVIEW_PV_VIEW_RENDERER_RENDER_WORKER_H

#include <QImage>
#include <QSize>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class QPainter;

namespace pv {
namespace view {

/**
 * RenderWorker — P1: background rasterization of the static layer (signal
 * waveforms) on a dedicated thread, published through a sequence-numbered
 * 3-slot triple buffer.
 *
 * The GUI thread builds a RenderWorker::Job whose ops are value-captured
 * lambdas (snapshot shared_ptr + all render params captured by value; each
 * op calls a rasterize_* pure function). The worker thread executes the ops
 * into an offscreen QImage, then publishes the completed frame with a
 * monotonic sequence number. paintEvent blits the newest completed frame and
 * draws the interactive layer (decode/cursors/measure) directly.
 *
 * Threading contract:
 *   - start()/stop()/submit()/try_acquire()/release()/set_published_callback()
 *     are MAIN-thread only.
 *   - The ops are executed ONLY on the worker thread.
 *   - try_acquire() returns a pointer to a published QImage that the caller
 *     must blit before calling release() (the worker never renders into an
 *     acquired slot, so the image is stable).
 *   - Published frames persist in the triple buffer until the worker recycles
 *     the oldest non-acquired frame for a newer job, so a completed frame
 *     remains blittable across paints (no forced rebuild).
 *   - If all 3 slots are busy when a job arrives, the oldest published
 *     (non-acquired) frame is recycled; the pending job is never dropped.
 */
class RenderWorker {
public:
  struct Job {
    QSize size;
    qreal dpr = 1.0;
    int vOffset = 0; // vertical scroll offset applied when rendering the frame
    // One op per channel. Each op captures its own params + snapshot
    // shared_ptr by value and calls a rasterize_* pure function.
    std::vector<std::function<void(QPainter &)>> ops;
  };

  RenderWorker();
  ~RenderWorker();

  void start();
  void stop();

  // Main thread. Coalesces: a newer submit replaces a not-yet-rendered one.
  void submit(Job job);

  // Main thread. If a completed frame matching (size, dpr) is available, set
  // *out to it and return true (caller MUST call release() after blitting).
  // Returns false only when no frame of this size is published yet (cold
  // start / resize). During interaction (zoom/scroll) the caller blits the
  // newest published frame — even though its view params may be one frame
  // stale — so the GUI thread never blocks on rasterization; the async frame
  // with the current params arrives ~1 frame later and triggers a repaint.
  bool try_acquire(const QSize &size, qreal dpr, const QImage **out);

  // Main thread. Release the frame acquired by try_acquire(). The published
  // frame STAYS published (seq is kept) so later paints can re-blit it
  // without forcing a rebuild; the worker recycles the oldest frame instead.
  void release();

  // Main thread. True if a published (non-acquired) frame matching (size, dpr)
  // is available in the triple buffer. Used by the paint gate to detect "no
  // valid cached frame of this size" (cold start / resize).
  bool has_frame(const QSize &size, qreal dpr);

  // Main thread. Callback invoked (on the worker thread) after a frame is
  // published; used to trigger a repaint on the GUI thread. Guarded by the
  // internal mutex; copy the value before calling cross-thread.
  void set_published_callback(std::function<void()> cb);

private:
  void run(); // worker thread body

  struct Frame {
    QImage image;
    uint64_t seq = 0;      // 0 = free; >0 = published (monotonic)
    bool acquired = false; // GUI is blitting it
    QSize size;
    qreal dpr = 1.0;
    int vOffset = 0;
    std::vector<std::function<void(QPainter &)>> ops;
  };

  std::thread _thread;
  std::mutex _mutex;
  std::condition_variable _cv;
  bool _running = false;
  bool _stop = false;
  Job _pending;
  bool _has_pending = false;
  Frame _frames[3];
  uint64_t _next_seq = 1;
  std::function<void()> _published_cb;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_RENDERER_RENDER_WORKER_H
