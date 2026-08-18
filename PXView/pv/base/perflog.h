#ifndef PXVIEW_PV_BASE_PERFLOG_H
#define PXVIEW_PV_BASE_PERFLOG_H

#include <chrono>
#include <map>
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

inline size_t    g_window_paint_calls   = 0;  // paint_mid calls this window
inline size_t    g_window_publish_calls = 0;  // publishes this window
inline size_t    g_frame_dense_rows     = 0;  // dense rows drawn this window
inline size_t    g_frame_mid_rows       = 0;  // mid rows drawn this window
inline uint64_t  g_frame_ann_sum        = 0;  // annotations-in-range this window
inline std::chrono::steady_clock::time_point g_perf_last_flush =
    std::chrono::steady_clock::now();

inline void record_frame_mid(double ms) {
  g_frame_mid.add(ms);
  g_window_paint_calls++;
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
  FILE *lf = fopen(path.toUtf8().constData(), "a");
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
