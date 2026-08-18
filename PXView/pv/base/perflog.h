#ifndef PXVIEW_PV_BASE_PERFLOG_H
#define PXVIEW_PV_BASE_PERFLOG_H

#include <chrono>
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
  double avg_u() const { return calls ? (double)sum_u64 / calls : 0; }
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
// P3-D7: main-thread sampling profiler histogram. A background watcher thread
// suspends the main thread every ~3ms and records its RIP; a long EVENT_LAG_MAX
// block that is not in any instrumented function shows up here as a dense
// cluster of samples. Printed as SAMPLES (resolve with addr2line), cleared
// each flush. Mutex-guarded; the sampler only takes the lock AFTER resuming
// the main thread, so it can never deadlock against a suspended main thread.
inline std::mutex g_sample_mutex;
inline std::map<uintptr_t, size_t> g_main_samples;
inline std::map<std::string, size_t> g_main_sample_modules;
inline void record_main_sample(uintptr_t rip) {
  std::lock_guard<std::mutex> lk(g_sample_mutex);
  g_main_samples[rip]++;
}
inline void record_main_sample_module(const std::string &mod) {
  std::lock_guard<std::mutex> lk(g_sample_mutex);
  g_main_sample_modules[mod]++;
}
inline std::map<uintptr_t, size_t> take_main_samples() {
  std::lock_guard<std::mutex> lk(g_sample_mutex);
  std::map<uintptr_t, size_t> out;
  out.swap(g_main_samples);
  return out;
}
inline std::map<std::string, size_t> take_main_sample_modules() {
  std::lock_guard<std::mutex> lk(g_sample_mutex);
  std::map<std::string, size_t> out;
  out.swap(g_main_sample_modules);
  return out;
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
// P3-D2: caller-address histogram for View::viewport_update(). Each call
// records __builtin_return_address(0); flush() prints the top callers so they
// can be mapped with addr2line against install.dir/bin/PXView.exe. Cleared per
// window. Guarded by PXVIEW_DECODE_PERF so there is zero cost otherwise.
inline std::map<void*, size_t> g_vp_callers;
inline void record_repaint_viewport_caller(void *caller) {
  g_repaint_viewport++;
  if (caller)
    g_vp_callers[caller]++;
}
// P3-D3: caller-address histogram for Viewport::update(int) — every direct
// viewport update() call. Printed as UPDATE_CALLERS; resolve with addr2line.
inline std::map<void*, size_t> g_ud_callers;
inline void record_repaint_update_direct_caller(void *caller) {
  g_repaint_update_direct++;
  if (caller)
    g_ud_callers[caller]++;
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
inline void record_frame_viewport(double ms) { g_frame_viewport.add(ms); }
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

inline void flush() {
  const auto now = std::chrono::steady_clock::now();
  const double since_s =
      std::chrono::duration<double>(now - g_perf_last_flush).count();
  // Throttle: flush at most roughly every 2s or 120 paint_mid calls.
  if (since_s < 2.0 && g_window_paint_calls < 120)
    return;
  g_perf_last_flush = now;
  const double paint_rate  = since_s > 0 ? (double)g_window_paint_calls / since_s : 0;
  const double publish_rate = since_s > 0 ? (double)g_window_publish_calls / since_s : 0;

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
  const double fpc = g_window_paint_calls ? (double)g_window_paint_calls : 1;
  fprintf(lf,
          "PER_FRAME_AVG   dense_rows=%.2f mid_rows=%.2f ann_in_range=%.1f\n",
          (double)g_frame_dense_rows / fpc, (double)g_frame_mid_rows / fpc,
          (double)g_frame_ann_sum / fpc);

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
          paints_window, (long long)paints_window - (long long)entries_sum);

  // P3-D4: max main-thread event-loop lag (100ms tick overshoot). 0 means the
  // GUI thread was never blocked; a large value = the freeze duration.
  fprintf(lf, "EVENT_LAG_MAX   %.1f ms\n", g_event_lag_max_ms);
  g_event_lag_max_ms = 0;
  g_event_lag_last = std::chrono::steady_clock::time_point{};

  // P3-F2: largest single publish delta this window (annotations).
  fprintf(lf, "MAX_PUBLISH_DELTA %zu ann\n", g_max_publish_delta);
  g_max_publish_delta = 0;

  // P3-D6: max process CPU util per 100ms tick this window (1.0 = one core).
  // High util (~n cores) alongside a large EVENT_LAG_MAX ⇒ decode threads
  // saturating the machine starve the GUI thread (not a main-thread op).
  fprintf(lf, "CPU_UTIL_MAX    %.1f cores\n", g_cpu_util_max);
  g_cpu_util_max = 0;

  // P3-D7: top main-thread sampled RIPs (sampling profiler). Dense clusters =
  // where the main thread spent its time (e.g. the EVENT_LAG_MAX block).
  {
    auto samples = take_main_samples();
    if (!samples.empty()) {
      std::vector<std::pair<uintptr_t, size_t>> sv(samples.begin(),
                                                   samples.end());
      std::sort(sv.begin(), sv.end(),
                [](const std::pair<uintptr_t, size_t> &a,
                   const std::pair<uintptr_t, size_t> &b) {
                  return a.second > b.second;
                });
      fprintf(lf, "SAMPLES:");
      const size_t top_s = std::min<size_t>(12, sv.size());
      for (size_t i = 0; i < top_s; i++)
        fprintf(lf, " 0x%p(x%zu)", (void *)sv[i].first, sv[i].second);
      fprintf(lf, "\n");
    }
    auto mods = take_main_sample_modules();
    if (!mods.empty()) {
      std::vector<std::pair<std::string, size_t>> mv(mods.begin(),
                                                      mods.end());
      std::sort(mv.begin(), mv.end(),
                [](const std::pair<std::string, size_t> &a,
                   const std::pair<std::string, size_t> &b) {
                  return a.second > b.second;
                });
      fprintf(lf, "SAMPLE_MODULES:");
      for (const auto &kv : mv)
        fprintf(lf, " %s=%zu", kv.first.c_str(), kv.second);
      fprintf(lf, "\n");
    }
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

  // P3-D2: top viewport_update() callers (return addresses, resolve with
  // addr2line -f -C <install.dir>/bin/PXView.exe <addr>).
  if (!g_vp_callers.empty()) {
    std::vector<std::pair<void*, size_t>> vp_sorted(g_vp_callers.begin(),
                                                    g_vp_callers.end());
    std::sort(vp_sorted.begin(), vp_sorted.end(),
              [](const std::pair<void*, size_t> &a,
                 const std::pair<void*, size_t> &b) {
                return a.second > b.second;
              });
    fprintf(lf, "VIEWPORT_CALLERS:");
    const size_t top_n = std::min<size_t>(8, vp_sorted.size());
    for (size_t i = 0; i < top_n; i++)
      fprintf(lf, " 0x%p(x%zu)", vp_sorted[i].first, vp_sorted[i].second);
    fprintf(lf, "\n");
    g_vp_callers.clear();
  }

  // P3-D3: top Viewport::update(int) callers (direct viewport update()).
  if (!g_ud_callers.empty()) {
    std::vector<std::pair<void*, size_t>> ud_sorted(g_ud_callers.begin(),
                                                    g_ud_callers.end());
    std::sort(ud_sorted.begin(), ud_sorted.end(),
              [](const std::pair<void*, size_t> &a,
                 const std::pair<void*, size_t> &b) {
                return a.second > b.second;
              });
    fprintf(lf, "UPDATE_CALLERS:");
    const size_t top_u = std::min<size_t>(8, ud_sorted.size());
    for (size_t i = 0; i < top_u; i++)
      fprintf(lf, " 0x%p(x%zu)", ud_sorted[i].first, ud_sorted[i].second);
    fprintf(lf, "\n");
    g_ud_callers.clear();
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
