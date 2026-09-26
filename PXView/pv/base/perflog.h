#ifndef PXVIEW_PV_BASE_PERFLOG_H
#define PXVIEW_PV_BASE_PERFLOG_H

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <QString>
#include <QDir>

// ----------------------------------------------------------------------------
// Decode / viewport render performance instrumentation.
//
// OFF unless compiled with -DPXVIEW_DECODE_PERF (CMake option
// ENABLE_DECODE_PERF). When ON, instruments:
//   * DecodeTrace::paint_mid()          -> per-frame + per-track-row timing
//   * ViewportPainter::paintEvent()     -> whole-viewport frame timing
//   * RowDataSnapshot::get_visible_range / RowData::get_visible_range -> lookup
//   * DecoderStack::publish_snapshot()  -> snapshot publish rate
//
// Aggregates are flushed periodically (~every 2s or 120 paint_mid calls) to
// %TEMP%/pxv_decode_perf.log, sorted by total track time. Designed to diagnose
// "many decoders + low zoom = janky" without any cost in normal builds.
//
// This header lives in pv/base (Core layer) so BOTH the Core data layer
// (rowdata.cpp, decoderstack.cpp) and the View layer (decodetrace.cpp,
// viewport_painter.cpp) can include it without violating layer boundaries.
// ----------------------------------------------------------------------------

#ifdef PXVIEW_DECODE_PERF

// P3-D8: heap topology probe needs Win32 heap APIs, glib (to sample g_malloc
// placement) and the Plan-A dedicated heap. All scoped to the perf macro so
// normal builds are untouched.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <glib.h>
#include <cstdlib>
#include "pv/data/decode/annotation_heap.h"

namespace pv {
namespace base {
namespace perf {

struct Agg {
  size_t   calls  = 0;
  double   total_ms = 0;
  double   max_ms   = 0;
  double   min_ms   = 1e9;
  double   aux1_ms  = 0;   // e.g. get_visible_range time
  double   aux2_ms  = 0;   // e.g. actual draw time
  uint64_t sum_u64  = 0;   // e.g. annotations in visible range

  void add(double ms) {
    calls++;
    total_ms += ms;
    if (ms > max_ms) max_ms = ms;
    if (ms < min_ms) min_ms = ms;
  }
  void add_row(double ms, double a1, double a2, uint64_t u) {
    calls++;
    total_ms += ms;
    aux1_ms  += a1;
    aux2_ms  += a2;
    sum_u64  += u;
    if (ms > max_ms) max_ms = ms;
    if (ms < min_ms) min_ms = ms;
  }
  double avg()   const { return calls ? total_ms / calls : 0; }
  double avg1()  const { return calls ? aux1_ms / calls : 0; }
  double avg2()  const { return calls ? aux2_ms / calls : 0; }
  double avg_u() const { return calls ? static_cast<double>(sum_u64) / calls : 0; }
};

// ---- global aggregates (inline, diagnostic-only; relaxed races OK) ----
inline Agg       g_frame_mid;        // DecodeTrace::paint_mid total
inline Agg       g_frame_viewport;   // Viewport whole-frame (all layers)
inline Agg       g_vrange_snap;      // RowDataSnapshot::get_visible_range
inline Agg       g_vrange_live;      // RowData::get_visible_range (live path)
inline std::map<QString, Agg> g_track;
// P3-D5: max-duration per named main-thread operation. Used to identify the
// single 1-1.4s EVENT_LAG_MAX block (e.g. signals_changed relayout vs
// on_new_decode_data snapshot swap vs publish_snapshot copy). Only the max
// per window is kept; printed as OP_MAX lines, cleared each flush.
inline std::map<QString, double> g_op_max;
inline void record_op_max(const char *name, double ms) {
  const QString key = QString::fromLatin1(name);
  auto it = g_op_max.find(key);
  if (it == g_op_max.end() || ms > it->second)
    g_op_max[key] = ms;
}
// P3-D6: max process-CPU utilization per 100ms tick in the window (1.0 = one
// core fully busy). Distinguishes "main thread starved by decode threads
// saturating all cores" (high util) from "main thread busy in an uninstrumented
// function" (low util). Sampled in View's 100ms event-lag timer.
inline double g_cpu_util_max = 0;
inline void record_cpu_util(double util) {
  if (util > g_cpu_util_max)
    g_cpu_util_max = util;
}
// P3-F2: largest single publish delta (annotations copied in one
// publish_snapshot call) this window. Confirms the heap-block hypothesis:
// million-annotation deltas were the source; after chunking the copy the
// delta can stay large but no single allocation blocks the heap.
inline size_t g_max_publish_delta = 0;
inline void record_publish_delta(size_t n) {
  if (n > g_max_publish_delta)
    g_max_publish_delta = n;
}
// P3-E: batch-annotation stats. In batch mode each flush delivers up to
// SRD_ANN_BATCH_MAX annotations with a single host callback; per-annotation
// engine-side g_malloc/g_free is eliminated, so approx_malloc_avoided ~=
// total annotations (each would have cost >= 1 heap alloc before).
inline size_t  g_batch_flushes  = 0;
inline uint64_t g_batch_ann_sum  = 0;
inline size_t  g_batch_max      = 0;
inline void record_batch_stats(size_t n) {
  g_batch_flushes++;
  g_batch_ann_sum += n;
  if (n > g_batch_max) g_batch_max = n;
}

// ---- P1-e: copy audit (devdoc/P0-P1-P2架构设计与实施计划.md) ----
// Counts the remaining whole-payload copies on the REAL capture / export paths,
// so the "copy distribution" can be measured at runtime instead of inferred
// from benchmarks.
//
//   staging : LogicSnapshotDiskCacheWriter 入队时的 memcpy（libsigrok 借用式
//             payload 契约导致，见 AGENTS/零拷贝调查）。P2 槽位池化后单核占用
//             已降到 ~1.8%，此计数器用于确认该结论在真实采集下成立。
//   export  : StoreSession / SessionService 导出时的交叉重打包（本就必要）。
//
// Reuses the existing ENABLE_DECODE_PERF gate rather than inventing a second
// one — zero cost in normal builds, no extra CMake option. Printed as
// COPY_AUDIT in the same %TEMP%/pxv_decode_perf.log window flush.
inline uint64_t g_copy_staging_calls = 0;
inline uint64_t g_copy_staging_bytes = 0;
inline uint64_t g_copy_export_calls  = 0;
inline uint64_t g_copy_export_bytes  = 0;
inline void record_copy_staging(uint64_t bytes) {
  g_copy_staging_calls++;
  g_copy_staging_bytes += bytes;
}
inline void record_copy_export(uint64_t bytes) {
  g_copy_export_calls++;
  g_copy_export_bytes += bytes;
}

// ---- P3-D8: heap topology probe ----
// Confirms the convoy premise: main-thread CRT malloc, Qt qMalloc and glib
// g_malloc all land on the SAME heap (one lock), while Plan A's per-stack
// HeapCreate heaps are genuinely separate. Uses HeapValidate(heap, 0, ptr) to
// find which heap owns a given pointer.
inline HANDLE probe_heap_of_pointer(const void *p) {
  if (!p)
    return nullptr;
  DWORD n = GetProcessHeaps(0, nullptr);
  if (!n || n > 64)
    return nullptr;
  HANDLE heaps[64];
  n = GetProcessHeaps(n, heaps);
  for (DWORD i = 0; i < n; i++)
    if (HeapValidate(heaps[i], 0, p))
      return heaps[i];
  return nullptr;
}
// Recorded from a decode thread: which heap a g_malloc'd block lands on there.
// If this equals the main-thread CRT heap, the 16 decode threads and the GUI
// thread share one heap lock (the convoy source). Relaxed write; diagnostic
// only.
inline HANDLE g_decode_thread_heap = nullptr;
inline void record_decode_thread_heap() {
  void *p = g_malloc(64);
  if (p) {
    HANDLE h = probe_heap_of_pointer(p);
    if (h)
      g_decode_thread_heap = h;
    g_free(p);
  }
}

inline size_t    g_window_paint_calls   = 0;  // paint_mid calls this window
inline size_t    g_window_publish_calls = 0;  // publishes this window
inline size_t    g_frame_dense_rows     = 0;  // dense rows drawn this window
inline size_t    g_frame_mid_rows       = 0;  // mid rows drawn this window
inline uint64_t  g_frame_ann_sum        = 0;  // annotations-in-range this window
inline std::chrono::steady_clock::time_point g_perf_last_flush =
    std::chrono::steady_clock::now();

// P3-D: repaint-source diagnostics. Counts the entry points that drive a
// viewport repaint so an "idle" ~54/s repaint (static scene) can be traced to
// its exact driver. Cross-checked against g_frame_viewport.calls in flush().
inline size_t g_repaint_progress      = 0;  // Viewport::on_progress_timer (repainting ticks)
inline size_t g_repaint_viewport      = 0;  // View::viewport_update() (full)
inline size_t g_repaint_decode_only   = 0;  // View::viewport_update_decode_only()
inline size_t g_repaint_delayed       = 0;  // View delayed-timer drains (any branch)
inline size_t g_repaint_delayed_full  = 0;  // ... drained as full viewport_update()
inline size_t g_repaint_delayed_do    = 0;  // ... drained as decode-only update
inline size_t g_repaint_data_updated  = 0;  // ViewDataSync::data_updated() (DataUpdated event)
inline size_t g_repaint_update_direct = 0;  // Viewport::update(int) — ANY direct viewport update() call
inline void record_repaint_progress()    { g_repaint_progress++; }
inline void record_repaint_viewport()    { g_repaint_viewport++; }
inline void record_repaint_decode_only() { g_repaint_decode_only++; }
inline void record_repaint_data_updated(){ g_repaint_data_updated++; }
inline void record_repaint_update_direct(){ g_repaint_update_direct++; }
inline void record_repaint_delayed(bool full) {
  g_repaint_delayed++;
  if (full) g_repaint_delayed_full++; else g_repaint_delayed_do++;
}
// g_frame_viewport.calls is CUMULATIVE across windows; remember the value at
// the previous flush so REPAINT_SOURCE can report the per-window delta.
inline size_t g_frame_viewport_last = 0;

inline void record_frame_mid(double ms) {
  g_frame_mid.add(ms);
  g_window_paint_calls++;
}

// P3-D4: main-thread event-loop lag. A 100ms QTimer on the GUI thread calls
// record_event_lag(); if the main thread is blocked (freeze), the tick arrives
// late and g_event_lag_max_ms captures the max delay per window. This is the
// decisive "is the GUI actually frozen / for how long" metric.
inline std::chrono::steady_clock::time_point g_event_lag_last{};
inline double g_event_lag_max_ms = 0;
inline void record_event_lag() {
  const auto now = std::chrono::steady_clock::now();
  if (g_event_lag_last.time_since_epoch().count() != 0) {
    const double delta_ms =
        std::chrono::duration<double, std::milli>(now - g_event_lag_last).count();
    // 100ms tick → a 200ms gap means the main thread was blocked ~100ms.
    const double lag_ms = delta_ms - 100.0;
    if (lag_ms > g_event_lag_max_ms)
      g_event_lag_max_ms = lag_ms;
  }
  g_event_lag_last = now;
}
// ---- Zoom animation trace: globals (定义在文件末尾 zoom_trace_* 处) ----
inline double g_last_frame_ms = 0;            // 最近一帧整帧耗时
inline Agg    g_zoom_frame;                   // 仅"缩放动画帧"的整帧耗时
inline bool   g_zoom_frame_pending = false;   // 下一次整帧绘制是否属于缩放动画

inline void record_frame_viewport(double ms) {
  g_frame_viewport.add(ms);
  // 供缩放动画追踪读取：把"最近一帧的整帧耗时"暴露出去，动画 tick 才能判断
  // 上一帧是否吃满了 16ms 帧预算（见 zoom_trace_frame）。同时，若上一 tick
  // 标记了"这一帧属于动画"，则计入动画专属聚合（与正常帧分离，避免被
  // 普通重绘稀释）。
  g_last_frame_ms = ms;
  if (g_zoom_frame_pending) {
    g_zoom_frame.add(ms);
    g_zoom_frame_pending = false;
  }
}
inline void record_vrange_snap(double ms)    { g_vrange_snap.add(ms); }
inline void record_vrange_live(double ms)    { g_vrange_live.add(ms); }
inline void record_publish()                 { g_window_publish_calls++; }
inline void record_track(const QString &name, double total_ms,
                         double vr_ms, double draw_ms, uint64_t ann) {
  g_track[name].add_row(total_ms, vr_ms, draw_ms, ann);
}
inline void frame_add_rows(size_t dense, size_t mid, uint64_t ann) {
  g_frame_dense_rows += dense;
  g_frame_mid_rows   += mid;
  g_frame_ann_sum    += ann;
}

// ---------------------------------------------------------------------------
// Zoom animation trace — 滚轮缩放动画（ZoomAnimation）
// ---------------------------------------------------------------------------
// 诊断目标："滚轮缩放手感迟钝"到底来自哪一条：
//   (1) 每帧重建信号 pixmap 的成本吃满帧预算
//       → 看 FRAME 行的 dt 与 prev_frame_ms：若 prev_frame_ms 接近或超过 dt，
//         说明每帧都在"画上一帧"，动画被绘制成本拖住。
//   (2) QTimer 的 16ms 量化 + 一帧调度延迟
//       → 看 dt_min 是否恒 ≈16ms（量化）而 dt_max 远大于它（偶发丢帧）；
//         若 dt_avg 明显小于 16ms 说明定时器不是瓶颈。
//   (3) 设备分流错误（触摸板被当成物理滚轮而走了 100ms 动画）
//       → 看 WHEEL 行的 physical= 判定与 pixel= 取值。
//
// 输出 %TEMP%/pxv_zoom_trace.log，每个滚轮手势一段 FRAME 行 + 一行 GESTURE
// 汇总（含 view_ms 范围 = 该手势期间视图可见时间跨度的最小/最大值）。
// 只在 PXVIEW_DECODE_PERF 下编译，正常构建零成本。
//
// ⚠ 所有 now_ms 必须来自同一个时钟（View::_zoom_anim_clock，QElapsedTimer），
//   否则 dt 计算会被污染。
inline size_t   g_zoom_gestures = 0;
inline size_t   g_zoom_ticks_total = 0;
inline double   g_zoom_tick_sum = 0, g_zoom_tick_max = 0, g_zoom_tick_min = 1e9;
inline double   g_zoom_viewms_min = 0, g_zoom_viewms_max = 0;
inline bool     g_zoom_viewms_seen = false;

struct ZoomTrace {
  bool    active = false;
  int     frames = 0;
  int64_t start_ms = 0, last_ms = 0;
  double  dt_sum = 0, dt_max = 0, dt_min = 1e9;
  double  frame_ms_sum = 0, frame_ms_max = 0;
  double  view_ms_min = 0, view_ms_max = 0;
};
inline ZoomTrace g_zoom_trace;

inline FILE *zoom_trace_file() {
  // 保持打开、逐行写入 + flush：手势很短（100~300ms，6~20 帧），且只在滚轮
  // 期间产生，I/O 开销可忽略；好处是短手势的日志不会被窗口 flush 节流丢掉。
  static FILE *f = nullptr;
  if (!f) {
    const QString p = QDir::temp().filePath("pxv_zoom_trace.log");
    f = fopen(p.toUtf8().constData(), "a");
  }
  return f;
}

// 一次滚轮手势开始（首次 retarget 时）。view_ms = 手势起点的可见时间跨度。
inline void zoom_trace_begin(int64_t now_ms, double view_ms) {
  ZoomTrace &z = g_zoom_trace;
  z = ZoomTrace{};
  z.active = true;
  z.start_ms = z.last_ms = now_ms;
  z.view_ms_min = z.view_ms_max = view_ms;
  if (FILE *f = zoom_trace_file())
    fprintf(f, "---- GESTURE BEGIN t=%lldms view_ms=%.3f ----\n",
            static_cast<long long>(now_ms), view_ms);
}

// 每个动画帧。progress = 线性进度 t(0..1)，view_ms = 本帧可见时间跨度。
inline void zoom_trace_frame(int64_t now_ms, double progress, double view_ms) {
  ZoomTrace &z = g_zoom_trace;
  if (!z.active)
    return;
  const double dt = static_cast<double>(now_ms - z.last_ms);
  z.last_ms = now_ms;
  z.frames++;
  z.dt_sum += dt;
  if (dt > z.dt_max) z.dt_max = dt;
  if (dt < z.dt_min) z.dt_min = dt;
  z.frame_ms_sum += g_last_frame_ms;
  if (g_last_frame_ms > z.frame_ms_max) z.frame_ms_max = g_last_frame_ms;
  if (view_ms < z.view_ms_min) z.view_ms_min = view_ms;
  if (view_ms > z.view_ms_max) z.view_ms_max = view_ms;
  // 标记"下一次整帧绘制属于动画"，由 record_frame_viewport 消费。
  g_zoom_frame_pending = true;
  if (FILE *f = zoom_trace_file())
    fprintf(f,
            "FRAME n=%d dt=%.1fms t=%.2f view_ms=%.3f prev_frame_ms=%.2f\n",
            z.frames, dt, progress, view_ms, g_last_frame_ms);
}

// 手势结束。now_ms 取 z.last_ms（与 dt 同源），故 wall 即"首帧到末帧"的真实
// 墙钟跨度，cancel 时略有低估（末帧之后才被取消）。
inline void zoom_trace_end(const char *reason, double view_ms) {
  ZoomTrace &z = g_zoom_trace;
  if (!z.active)
    return;
  z.active = false;
  if (view_ms < z.view_ms_min) z.view_ms_min = view_ms;
  if (view_ms > z.view_ms_max) z.view_ms_max = view_ms;

  g_zoom_gestures++;
  g_zoom_ticks_total += static_cast<size_t>(z.frames);
  g_zoom_tick_sum += z.dt_sum;
  if (z.dt_max > g_zoom_tick_max) g_zoom_tick_max = z.dt_max;
  if (z.frames > 0 && z.dt_min < g_zoom_tick_min) g_zoom_tick_min = z.dt_min;
  if (!g_zoom_viewms_seen) {
    g_zoom_viewms_min = z.view_ms_min;
    g_zoom_viewms_max = z.view_ms_max;
    g_zoom_viewms_seen = true;
  } else {
    if (z.view_ms_min < g_zoom_viewms_min) g_zoom_viewms_min = z.view_ms_min;
    if (z.view_ms_max > g_zoom_viewms_max) g_zoom_viewms_max = z.view_ms_max;
  }

  if (FILE *f = zoom_trace_file()) {
    fprintf(f,
            "GESTURE reason=%s frames=%d wall=%.1fms"
            " dt[min=%.1f avg=%.1f max=%.1f]"
            " frame_ms[avg=%.2f max=%.2f]"
            " view_ms=[%.3f..%.3f] range=%.3fms\n",
            reason, z.frames, static_cast<double>(z.last_ms - z.start_ms),
            z.frames > 0 ? z.dt_min : 0.0,
            z.frames > 0 ? z.dt_sum / z.frames : 0.0, z.dt_max,
            z.frames > 0 ? z.frame_ms_sum / z.frames : 0.0, z.frame_ms_max,
            z.view_ms_min, z.view_ms_max, z.view_ms_max - z.view_ms_min);
    fflush(f);
  }
}

// 滚轮事件分流诊断（viewport_interaction wheelEvent）。
inline void wheel_route_log(int angle_x, int angle_y, int pixel_x, int pixel_y,
                            bool physical, bool synthesized, const char *path) {
  if (FILE *f = zoom_trace_file())
    fprintf(f,
            "WHEEL angleX=%d angleY=%d pixel=(%d,%d) physical=%d synth=%d"
            " path=%s\n",
            angle_x, angle_y, pixel_x, pixel_y, physical ? 1 : 0,
            synthesized ? 1 : 0, path);
}

inline void flush() {
  const auto now = std::chrono::steady_clock::now();
  const double since_s =
      std::chrono::duration<double>(now - g_perf_last_flush).count();
  // Throttle: flush at most roughly every 2s or 120 paint_mid calls.
  if (since_s < 2.0 && g_window_paint_calls < 120)
    return;
  g_perf_last_flush = now;
  const double paint_rate  = since_s > 0 ? static_cast<double>(g_window_paint_calls) / since_s : 0;
  const double publish_rate = since_s > 0 ? static_cast<double>(g_window_publish_calls) / since_s : 0;

  QString path = QDir::temp().filePath("pxv_decode_perf.log");
  // Truncate the log on the FIRST flush of this process (each PXView launch
  // starts a fresh log), then append for subsequent flushes of the same run.
  static bool s_first_flush = true;
  const char *mode = s_first_flush ? "w" : "a";
  s_first_flush = false;
  FILE *lf = fopen(path.toUtf8().constData(), mode);
  if (!lf) return;

  fprintf(lf, "================ WINDOW (%.2fs) ================\n", since_s);

  // Whole-frame vs decode-only: the gap is the cost of everything else
  // (waveforms, cursors, overlays) drawn outside paint_mid.
  fprintf(lf,
          "FRAME_VIEWPORT  calls=%zu total=%.3fms max=%.3fms min=%.3fms avg=%.3fms\n",
          g_frame_viewport.calls, g_frame_viewport.total_ms,
          g_frame_viewport.max_ms, g_frame_viewport.min_ms,
          g_frame_viewport.avg());
  fprintf(lf,
          "FRAME_PAINTMID  calls=%zu total=%.3fms max=%.3fms min=%.3fms avg=%.3fms"
          "  PAINT_RATE=%.1f/s\n",
          g_frame_mid.calls, g_frame_mid.total_ms, g_frame_mid.max_ms,
          g_frame_mid.min_ms, g_frame_mid.avg(), paint_rate);

  // Average per-frame composition (dense vs mid rows, annotations drawn).
  const double fpc = g_window_paint_calls ? static_cast<double>(g_window_paint_calls) : 1;
  fprintf(lf,
          "PER_FRAME_AVG   dense_rows=%.2f mid_rows=%.2f ann_in_range=%.1f\n",
          static_cast<double>(g_frame_dense_rows) / fpc, static_cast<double>(g_frame_mid_rows) / fpc,
          static_cast<double>(g_frame_ann_sum) / fpc);

  // Range-lookup cost (the correctness-fixed get_visible_range).
  fprintf(lf,
          "VRANGE_SNAPSHOT calls=%zu total=%.3fms max=%.3fms avg=%.4fms\n",
          g_vrange_snap.calls, g_vrange_snap.total_ms, g_vrange_snap.max_ms,
          g_vrange_snap.avg());
  fprintf(lf,
          "VRANGE_LIVE     calls=%zu total=%.3fms max=%.3fms avg=%.4fms\n",
          g_vrange_live.calls, g_vrange_live.total_ms, g_vrange_live.max_ms,
          g_vrange_live.avg());

  // Snapshot publish rate (decode-thread -> GUI invalidate pressure).
  fprintf(lf, "PUBLISH_SNAPSHOT rate=%.1f/s total=%zu\n",
          publish_rate, g_window_publish_calls);

  // P3-D: repaint-source breakdown. The per-window paint count is the DELTA of
  // g_frame_viewport.calls (cumulative) since the last flush. Comparing it to
  // the sum of the entry counters reveals repaints that arrive through an
  // UNCOUNTERED path (e.g. a self-sustaining paint loop):
  //   gap == 0        -> the counters explain every paint
  //   gap >> 0        -> a repaint source not covered by the counters
  const double prog_rate  = since_s > 0 ? g_repaint_progress       / since_s : 0;
  const double vp_rate    = since_s > 0 ? g_repaint_viewport       / since_s : 0;
  const double do_rate    = since_s > 0 ? g_repaint_decode_only    / since_s : 0;
  const double dl_rate    = since_s > 0 ? g_repaint_delayed        / since_s : 0;
  const double du_rate    = since_s > 0 ? g_repaint_data_updated   / since_s : 0;
  const double ud_rate    = since_s > 0 ? g_repaint_update_direct  / since_s : 0;
  const size_t paints_window = g_frame_viewport.calls - g_frame_viewport_last;
  g_frame_viewport_last = g_frame_viewport.calls;
  const size_t entries_sum =
      g_repaint_progress + g_repaint_viewport + g_repaint_decode_only +
      g_repaint_delayed + g_repaint_data_updated + g_repaint_update_direct;
  fprintf(lf,
          "REPAINT_SOURCE progress=%.1f/s(%zu) viewport=%.1f/s(%zu)"
          " decode_only=%.1f/s(%zu) data_updated=%.1f/s(%zu)"
          " update_direct=%.1f/s(%zu) delayed=%.1f/s(%zu) [full=%zu do=%zu]"
          " sum=%zu paints_delta=%zu gap=%zd\n",
          prog_rate, g_repaint_progress, vp_rate, g_repaint_viewport,
          do_rate, g_repaint_decode_only, du_rate, g_repaint_data_updated,
          ud_rate, g_repaint_update_direct, dl_rate, g_repaint_delayed,
          g_repaint_delayed_full, g_repaint_delayed_do, entries_sum,
          paints_window, static_cast<long long>(paints_window) - static_cast<long long>(entries_sum));

  // P3-D4: max main-thread event-loop lag (100ms tick overshoot). 0 means the
  // GUI thread was never blocked; a large value = the freeze duration.
  fprintf(lf, "EVENT_LAG_MAX   %.1f ms\n", g_event_lag_max_ms);
  g_event_lag_max_ms = 0;
  g_event_lag_last = std::chrono::steady_clock::time_point{};

  // P3-F2: largest single publish delta this window (annotations).
  fprintf(lf, "MAX_PUBLISH_DELTA %zu ann\n", g_max_publish_delta);
  g_max_publish_delta = 0;

  // P3-E: batch-annotation pipeline stats. batches = engine->host flushes,
  // total_ann = annotations delivered in this window, avg/max = per-batch
  // size, approx_malloc_avoided ~= total annotations (each would have cost
  // >= 1 heap alloc with the per-annotation callback before).
  fprintf(lf,
          "BATCH_STATS   batches=%zu total_ann=%llu avg=%.1f max=%zu"
          " approx_malloc_avoided=%llu\n",
          g_batch_flushes, (unsigned long long)g_batch_ann_sum,
          g_batch_flushes ? static_cast<double>(g_batch_ann_sum) / g_batch_flushes : 0.0,
          g_batch_max, (unsigned long long)g_batch_ann_sum);
  g_batch_flushes = 0;
  g_batch_ann_sum = 0;
  g_batch_max = 0;

  // P1-e: copy audit — remaining whole-payload copies on the capture/export paths.
#ifdef PXVIEW_COPY_AUDIT
  fprintf(lf,
          "COPY_AUDIT    staging_calls=%llu staging_MB=%.1f"
          " export_calls=%llu export_MB=%.1f\n",
          (unsigned long long)g_copy_staging_calls,
          static_cast<double>(g_copy_staging_bytes) / (1024.0 * 1024.0),
          (unsigned long long)g_copy_export_calls,
          static_cast<double>(g_copy_export_bytes) / (1024.0 * 1024.0));
  g_copy_staging_calls = 0;
  g_copy_staging_bytes = 0;
  g_copy_export_calls  = 0;
  g_copy_export_bytes  = 0;
#endif

  // Zoom animation (滚轮缩放动画): 动画帧的整帧耗时与 tick 节奏。
  // anim_frame 只统计"缩放动画进行中"的帧（普通重绘不进这个聚合），
  // 因此它的 avg/max 就是"动画每帧要花多少 ms"；把它和 tick avg 比：
  //   anim_frame.avg 接近 tick.avg  -> 绘制吃满帧预算，动画被拖慢（要降 LOD）
  //   anim_frame.avg 远小于 tick.avg -> 瓶颈在定时器量化/调度，不在绘制
  fprintf(lf,
          "ZOOM_ANIM      gestures=%zu ticks=%zu anim_frame[calls=%zu avg=%.2fms"
          " max=%.2fms] tick[min=%.1f avg=%.1f max=%.1f]ms"
          " view_ms=[%.3f..%.3f] range=%.3fms\n",
          g_zoom_gestures, g_zoom_ticks_total, g_zoom_frame.calls,
          g_zoom_frame.avg(), g_zoom_frame.max_ms,
          (g_zoom_ticks_total && g_zoom_tick_min < 1e9) ? g_zoom_tick_min : 0.0,
          g_zoom_ticks_total ? g_zoom_tick_sum / g_zoom_ticks_total : 0.0,
          g_zoom_tick_max, g_zoom_viewms_min, g_zoom_viewms_max,
          g_zoom_viewms_max - g_zoom_viewms_min);
  g_zoom_gestures = 0;
  g_zoom_ticks_total = 0;
  g_zoom_tick_sum = 0;
  g_zoom_tick_max = 0;
  g_zoom_tick_min = 1e9;
  g_zoom_viewms_min = 0;
  g_zoom_viewms_max = 0;
  g_zoom_viewms_seen = false;

  // P3-D6: max process CPU util per 100ms tick this window (1.0 = one core).
  // High util (~n cores) alongside a large EVENT_LAG_MAX ⇒ decode threads
  // saturating the machine starve the GUI thread (not a main-thread op).
  fprintf(lf, "CPU_UTIL_MAX    %.1f cores\n", g_cpu_util_max);
  g_cpu_util_max = 0;

  // P3-D8: heap topology — where each allocation source actually lands.
  // Equal heap handles (crt/qt/glib) = one shared lock = convoy confirmed;
  // planA != crt = Plan-A heaps genuinely separate; decode_thread_glib == crt
  // = decode threads allocate on the SAME heap as the GUI thread.
  {
    // P3-D8b: probe C++ operator new (std::vector during paint/annotation copy)
    // instead of Qt — Qt 6.11 only exposes qMallocAligned (aligned pointer sits
    // past the block base, so HeapValidate returns 0 — an artifact, not a
    // different heap; Qt on MinGW allocates through the same CRT malloc).
    void *p_crt = ::malloc(64);
    void *p_cxx = ::operator new(64);
    void *p_glib = g_malloc(64);
    const HANDLE h_crt = probe_heap_of_pointer(p_crt);
    const HANDLE h_cxx = probe_heap_of_pointer(p_cxx);
    const HANDLE h_glib = probe_heap_of_pointer(p_glib);
    HANDLE h_planA = nullptr;
    void *heapA = pv::data::decode::create_annotation_heap();
    if (heapA) {
      void *pa = pv::data::decode::annotation_heap_alloc(heapA, 64);
      if (pa)
        h_planA = probe_heap_of_pointer(pa);
      pv::data::decode::annotation_heap_free(heapA, pa);
      pv::data::decode::destroy_annotation_heap(heapA);
    }
    ::free(p_crt);
    ::operator delete(p_cxx);
    g_free(p_glib);
    fprintf(lf,
            "HEAP_TOPOLOGY process=0x%p crt_malloc=0x%p"
            " cxx_new=0x%p glib=0x%p"
            " planA=0x%p decode_thread_glib=0x%p\n",
            GetProcessHeap(), h_crt, h_cxx, h_glib, h_planA,
            g_decode_thread_heap);
  }

  // P3-D5: max duration of the instrumented main-thread operations, to pin
  // down the EVENT_LAG_MAX block source.
  if (!g_op_max.empty()) {
    fprintf(lf, "OP_MAX:");
    for (const auto &kv : g_op_max)
      fprintf(lf, " %s=%.1fms", kv.first.toUtf8().constData(), kv.second);
    fprintf(lf, "\n");
    g_op_max.clear();
  }

  // Per-track-row breakdown, hottest first.
  fprintf(lf, "TRACK ROWS (%zu):\n", g_track.size());
  std::vector<std::pair<QString, Agg>> sorted(g_track.begin(), g_track.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const std::pair<QString, Agg> &a,
               const std::pair<QString, Agg> &b) {
              return a.second.total_ms > b.second.total_ms;
            });
  for (const auto &kv : sorted) {
    const Agg &a = kv.second;
    fprintf(lf,
            "  %-26s calls=%zu total=%.3fms max=%.3fms avg=%.3fms"
            " | vrange_avg=%.4fms draw_avg=%.4fms ann_avg=%.1f\n",
            qPrintable(kv.first), a.calls, a.total_ms, a.max_ms, a.avg(),
            a.avg1(), a.avg2(), a.avg_u());
  }
  fprintf(lf, "\n");
  fclose(lf);

  // reset window counters
  g_window_paint_calls   = 0;
  g_window_publish_calls = 0;
  g_frame_dense_rows     = 0;
  g_frame_mid_rows       = 0;
  g_frame_ann_sum        = 0;
  g_repaint_progress     = 0;
  g_repaint_viewport     = 0;
  g_repaint_decode_only  = 0;
  g_repaint_delayed      = 0;
  g_repaint_delayed_full = 0;
  g_repaint_delayed_do   = 0;
  g_repaint_data_updated = 0;
  g_repaint_update_direct = 0;
}

// ---- scoped RAII timer for the whole viewport frame ----
struct _PerfScope {
  bool on;
  std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  explicit _PerfScope(bool o) : on(o) {}
  ~_PerfScope() {
    if (!on) return;
    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    record_frame_viewport(ms);
  }
};

}  // namespace perf
}  // namespace base
}  // namespace pv

// Whole-viewport frame scope (place at top of ViewportPainter::paintEvent).
#define PXV_PERF_SCOPE_VIEWPORT() \
  pv::base::perf::_PerfScope _perf_vp_scope(true)

// paint_mid frame scope: records + triggers periodic flush.
#define PXV_PERF_PAINTMID_START() \
  const auto _perf_pm_t0 = std::chrono::steady_clock::now();
#define PXV_PERF_PAINTMID_END()                                          \
  do {                                                                   \
    double _perf_pm_ms = std::chrono::duration<double, std::milli>(       \
        std::chrono::steady_clock::now() - _perf_pm_t0).count();         \
    pv::base::perf::record_frame_mid(_perf_pm_ms);                       \
    pv::base::perf::flush();                                             \
  } while (0)

#else  // !PXVIEW_DECODE_PERF

// Zoom trace: 关闭插桩时全部退化为空操作，调用点无需 #ifdef。
namespace pv {
namespace base {
namespace perf {
inline void zoom_trace_begin(int64_t, double) {}
inline void zoom_trace_frame(int64_t, double, double) {}
inline void zoom_trace_end(const char *, double) {}
inline void wheel_route_log(int, int, int, int, bool, bool, const char *) {}
}  // namespace perf
}  // namespace base
}  // namespace pv

#define PXV_PERF_SCOPE_VIEWPORT() \
  do {                            \
  } while (0)
#define PXV_PERF_PAINTMID_START() \
  do {                            \
  } while (0)
#define PXV_PERF_PAINTMID_END() \
  do {                          \
  } while (0)
#endif

#endif  // PXVIEW_PV_BASE_PERFLOG_H
