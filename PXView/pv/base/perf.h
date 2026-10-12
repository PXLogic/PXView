#ifndef PXVIEW_PV_BASE_PERF_H
#define PXVIEW_PV_BASE_PERF_H

// ============================================================================
// PXView perf instrumentation — metric registry + pluggable sinks.
//
// Replaces the ad-hoc global counters plus hand-written fprintf() flush that
// used to live in pv/base/perflog.h (now deleted; this header is the only
// perf entry point). Design rules:
//
//   * ONE compile gate (PXV_PERF, CMake option ENABLE_PERF, default OFF) and
//     ONE runtime level (Off/Basic/Verbose). With the gate off every PXV_PERF_*
//     macro expands to a no-op — no atomic, no branch, no symbol — so normal
//     builds pay literally nothing and keep the documented zero-cost promise.
//   * Metrics are declared ONCE in the kMetrics table. The declaration order
//     IS the MetricId order IS the storage index, so a hot-path sample is a
//     single array index: no string lookup, no allocation.
//   * Hot path uses relaxed atomics. A relaxed load/store is a plain mov, but
//     fetch_add is `lock xadd` (~20+ cycles, locks the cache line) — if a
//     per-item counter is ever added from the 16 decode threads, shard it
//     per-thread and merge at snapshot instead of sharing one atomic. The
//     metrics present today are all per-frame / per-batch, so plain atomics
//     are fine.
//   * Output is decoupled behind IPerfSink. No metric code knows about any
//     file path or output format.
//
// Layer: Core (pv/base, compiled into pxview-core). Must NOT pull in Qt
// Widgets — pxviewd links this library. Deliberately Qt-free (std::string +
// std::chrono) so it is also usable from unit tests without a Qt app object.
// ============================================================================

// ---------------------------------------------------------------------------
// No-op expansions. Defined unconditionally so call sites never need an
// #ifdef/#else and never need a (void) dance. Arguments are deliberately NOT
// evaluated (except a plain (void) cast to keep locals "used"), so a disabled
// build really does compile the sampling away.
// ---------------------------------------------------------------------------
#ifdef PXV_PERF

#define PXV_PERF_ADD(id, n) \
  ::pv::base::perf::add((id), static_cast<uint64_t>(n))
#define PXV_PERF_GAUGE(id, v) \
  ::pv::base::perf::set_gauge((id), static_cast<double>(v))
#define PXV_PERF_OBSERVE(id, ms) \
  ::pv::base::perf::observe((id), static_cast<double>(ms))
#define PXV_PERF_SCOPE(id) \
  ::pv::base::perf::ScopedTimer _pxv_perf_scope_((id))
#define PXV_PERF_TRACK_ROW(name) \
  ::pv::base::perf::TrackRowScope _pxv_perf_row_((name))
#define PXV_PERF_TRACK_AUX_NS(ns) \
  _pxv_perf_row_.add_aux_ns(static_cast<uint64_t>(ns))
#define PXV_PERF_TRACK_COUNT(n) \
  _pxv_perf_row_.add_count(static_cast<uint64_t>(n))
// Variadic on purpose: the expression routinely contains a comma
// (std::chrono::duration<double, std::milli>), which the preprocessor would
// otherwise split into extra macro arguments.
#define PXV_PERF_OP_MAX(name, ...) \
  ::pv::base::perf::op_max((name), static_cast<double>(__VA_ARGS__))
#define PXV_PERF_TRACK_AUX_MS(ms) \
  _pxv_perf_row_.add_aux_ms(static_cast<double>(ms))
#define PXV_PERF_TRACK_ROW_VALUES(name, total_ms, aux_ms, count) \
  do { \
    ::pv::base::perf::track_row((name), static_cast<double>(total_ms), \
                                static_cast<double>(aux_ms), \
                                static_cast<std::uint64_t>(count)); \
  } while (0)
#define PXV_PERF_TRACK_COUNT_ONCE(name, n) \
  ::pv::base::perf::track_row((name), 0.0, 0.0, static_cast<std::uint64_t>(n))
#define PXV_PERF_HEARTBEAT() ::pv::base::perf::heartbeat()
#define PXV_PERF_ZOOM_FRAME() \
  (::pv::base::perf::zoom_frame_pending() = true)
#define PXV_PERF_COPY_STAGING(bytes) \
  do { \
    ::pv::base::perf::add(::pv::base::perf::MetricId::CopyStagingCalls, 1); \
    ::pv::base::perf::add(::pv::base::perf::MetricId::CopyStagingBytes, \
                          static_cast<std::uint64_t>(bytes)); \
  } while (0)
#define PXV_PERF_COPY_EXPORT(bytes) \
  do { \
    ::pv::base::perf::add(::pv::base::perf::MetricId::CopyExportCalls, 1); \
    ::pv::base::perf::add(::pv::base::perf::MetricId::CopyExportBytes, \
                          static_cast<std::uint64_t>(bytes)); \
  } while (0)

#else  // !PXV_PERF

#define PXV_PERF_ADD(id, n)      do { (void)(n); } while (0)
#define PXV_PERF_GAUGE(id, v)    do { (void)(v); } while (0)
#define PXV_PERF_OBSERVE(id, ms) do { (void)(ms); } while (0)
#define PXV_PERF_SCOPE(id)       do { } while (0)
#define PXV_PERF_TRACK_ROW(name) do { (void)(name); } while (0)
#define PXV_PERF_TRACK_AUX_NS(ns) do { (void)(ns); } while (0)
#define PXV_PERF_TRACK_COUNT(n)   do { (void)(n); } while (0)
#define PXV_PERF_OP_MAX(name, ...) \
  do { (void)(name); (void)sizeof((__VA_ARGS__)); } while (0)
#define PXV_PERF_TRACK_AUX_MS(ms) do { (void)(ms); } while (0)
#define PXV_PERF_TRACK_ROW_VALUES(name, total_ms, aux_ms, count) \
  do { (void)(name); (void)(total_ms); (void)(aux_ms); (void)(count); } while (0)
#define PXV_PERF_TRACK_COUNT_ONCE(name, n) do { (void)(name); (void)(n); } while (0)
#define PXV_PERF_HEARTBEAT() do { } while (0)
#define PXV_PERF_ZOOM_FRAME() do { } while (0)
#define PXV_PERF_COPY_STAGING(bytes) do { (void)(bytes); } while (0)
#define PXV_PERF_COPY_EXPORT(bytes) do { (void)(bytes); } while (0)

#endif  // PXV_PERF

#ifdef PXV_PERF

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace pv {
namespace base {
namespace perf {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Runtime verbosity. Only meaningful in a PXV_PERF build: the compile gate
// decides whether the code exists at all, the level decides how much of it is
// active without a rebuild.
// ---------------------------------------------------------------------------
enum class Level : std::uint8_t {
  Off = 0,    // sampling compiled in but inert
  Basic = 1,  // default when PXV_PERF is on
  Verbose = 2 // per-row / per-track breakdowns
};

Level level() noexcept;
void set_level(Level l) noexcept;
bool enabled() noexcept;           // level() != Level::Off
void init_from_env() noexcept;     // reads PXVIEW_PERF_LEVEL

// ---------------------------------------------------------------------------
// Metric declaration.
//
// Kind decides how the sample is stored and how the window is reduced:
//   Counter — PXV_PERF_ADD(id, n): calls++, sum += n
//   Gauge   — PXV_PERF_GAUGE(id, v): calls++, last = v, max/min tracked
//   Timer   — PXV_PERF_OBSERVE(id, ms): calls++, sum += ms, max/min tracked
// ---------------------------------------------------------------------------
enum class Kind : std::uint8_t { Counter, Gauge, Timer };
enum class Agg : std::uint8_t { Sum, Avg, Max, Min, Last };

enum class MetricId : std::uint16_t {
  // ---- frame / render ----
  FrameViewport,       // whole-viewport paint
  FramePaintMid,       // decode-track painting only
  FrameInterval,       // wall time between delivered frames (see frame_boundary)
  PaintCalls,          // frames delivered in this window
  DenseRows,           // dense rows drawn
  MidRows,             // mid (annotation-block) rows drawn
  AnnInRange,          // annotations in visible range
  // ---- range lookup ----
  VrangeSnap,
  VrangeLive,
  // ---- decode pipeline ----
  PublishCount,
  MaxPublishDelta,
  BatchFlushes,
  BatchAnn,
  BatchMax,
  // ---- copy audit ----
  CopyStagingCalls,
  CopyStagingBytes,
  CopyExportCalls,
  CopyExportBytes,
  // ---- event loop health ----
  EventLagMax,         // max overshoot of the heartbeat tick, ms
  CpuUtilMax,          // max process CPU utilisation per tick, in cores
  // ---- repaint source accounting ----
  RepaintProgress,
  RepaintViewport,
  RepaintDecodeOnly,
  RepaintDataUpdated,
  RepaintUpdateDirect,
  RepaintDelayed,
  RepaintDelayedFull,
  RepaintDelayedDecodeOnly,
  // ---- interaction ----
  ZoomGestures,
  ZoomTicks,
  ZoomAnimFrame,
  // ---- frame rate (sliding 1s window, NOT tied to the summary window) ----
  FrameFps,          // whole-app delivered frames/s (top-level UpdateRequest)
  FrameViewportFps,  // viewport repaints/s

  Count_  // sentinel — must stay last
};

struct MetricDef {
  const char *name;  // stable machine id, e.g. "frame.viewport"
  const char *unit;  // "ms" | "count" | "cores" | "bytes" | "ann"
  Kind kind;
  Agg agg;
  const char *desc;
};

std::size_t metric_count() noexcept;
const MetricDef &metric_def(MetricId id) noexcept;
MetricId metric_by_name(const char *name) noexcept;  // Count_ if unknown

// ---------------------------------------------------------------------------
// Hot-path sampling.
// ---------------------------------------------------------------------------
void add(MetricId id, std::uint64_t n) noexcept;
void set_gauge(MetricId id, double v) noexcept;
void observe(MetricId id, double ms) noexcept;

// ---------------------------------------------------------------------------
// Named (low-frequency) metrics. These do NOT fit a fixed enum because the
// key set is open (decoder row titles, main-thread op names). They are sampled
// per-frame / per-operation, never per-item, so a mutex-guarded map is fine.
// ---------------------------------------------------------------------------
void track_row(const char *name, double total_ms, double aux_ms,
               std::uint64_t count) noexcept;
void op_max(const char *name, double ms) noexcept;

// ---------------------------------------------------------------------------
// Window snapshot. take_snapshot(reset=true) is the window roll: it reads and
// clears every accumulator so consecutive windows do not overlap.
// ---------------------------------------------------------------------------
struct MetricSample {
  MetricId id;
  std::uint64_t calls;
  double total;  // sum over the window
  double max;
  double min;
  double last;
  double value;  // reduced per Agg: sum / avg / max / min / last
};

struct NamedSample {
  std::string name;
  std::uint64_t calls;
  double total;
  double max;
  double aux;     // e.g. get_visible_range time for a track row
  std::uint64_t count;
};

struct Snapshot {
  double window_s = 0.0;
  std::vector<MetricSample> metrics;
  std::vector<NamedSample> rows;  // track rows, hottest first
  std::vector<NamedSample> ops;   // op maxima, by name
};

Snapshot take_snapshot(bool reset) noexcept;

// ---------------------------------------------------------------------------
// Sinks.
// ---------------------------------------------------------------------------
class IPerfSink {
public:
  virtual ~IPerfSink();
  virtual const char *name() const = 0;
  virtual void on_window(const Snapshot &s) = 0;
};

void add_sink(IPerfSink *sink) noexcept;
void remove_sink(IPerfSink *sink) noexcept;

// Builds a snapshot (resetting the window), then fans it out to every sink.
// Called on a throttle by frame_boundary() / the headless timer.
void flush_window() noexcept;

// ---------------------------------------------------------------------------
// Drivers. The Core layer has no event loop of its own, so the host calls
// these:
//   GUI      — a QEvent::UpdateRequest filter on the top-level widget
//              (measured: UpdateRequest is delivered to the top-level QWidget,
//              never to the QWindow, and it covers every child repaint).
//   headless — a QTimer in the daemon loop.
// ---------------------------------------------------------------------------
void frame_boundary() noexcept;  // frame interval + throttled flush
void heartbeat() noexcept;       // event-loop lag sampling (call every ~10ms)
// Called by the viewport frame scope so frame.viewport_fps can be derived.
void note_viewport_frame() noexcept;
void set_flush_interval_ms(int ms) noexcept;
void set_heartbeat_interval_ms(int ms) noexcept;
void set_frame_idle_ms(int ms) noexcept;

// ---------------------------------------------------------------------------
// Scoped helpers (what the macros expand to).
// ---------------------------------------------------------------------------
class ScopedTimer {
public:
  explicit ScopedTimer(MetricId id) noexcept : _id(id), _on(enabled()) {
    if (_on)
      _t0 = Clock::now();
  }
  ~ScopedTimer() noexcept {
    if (_on)
      observe(_id, ms_since(_t0));
  }
  ScopedTimer(const ScopedTimer &) = delete;
  ScopedTimer &operator=(const ScopedTimer &) = delete;

private:
  static double ms_since(Clock::time_point t0) noexcept;
  MetricId _id;
  bool _on;
  Clock::time_point _t0{};
};

class TrackRowScope {
public:
  explicit TrackRowScope(const char *name) noexcept : _name(name), _on(enabled()) {
    if (_on)
      _t0 = Clock::now();
  }
  ~TrackRowScope() noexcept;
  void add_aux_ns(std::uint64_t ns) noexcept {
    if (_on)
      _aux_ns += ns;
  }
  void add_aux_ms(double ms) noexcept {
    if (_on)
      _aux_ns += static_cast<std::uint64_t>(ms * 1e6 + 0.5);
  }
  void add_count(std::uint64_t n) noexcept {
    if (_on)
      _count += n;
  }
  TrackRowScope(const TrackRowScope &) = delete;
  TrackRowScope &operator=(const TrackRowScope &) = delete;

private:
  const char *_name;
  bool _on;
  Clock::time_point _t0{};
  std::uint64_t _aux_ns = 0;
  std::uint64_t _count = 0;
};

// Nanoseconds elapsed since a Clock::time_point (for call sites that measure
// an auxiliary span themselves, e.g. the get_visible_range sub-timing).
std::uint64_t ns_since(Clock::time_point t0) noexcept;

// A frame painted while a wheel-zoom gesture is running is attributed to
// ZoomAnimFrame as well, so the animation frame cost stays separable from
// ordinary repaints. PXV_PERF_ZOOM_FRAME() arms it; ViewportScope consumes it.
bool &zoom_frame_pending() noexcept;

// Viewport frame scope. A dedicated RAII type (rather than PXV_PERF_SCOPE)
// because it also has to consume the zoom-animation frame marker.
class ViewportScope {
public:
  ViewportScope() noexcept : _on(enabled()) {
    if (_on)
      _t0 = Clock::now();
  }
  ~ViewportScope() noexcept {
    if (!_on)
      return;
    const double ms = static_cast<double>(ns_since(_t0)) / 1e6;
    observe(MetricId::FrameViewport, ms);
    note_viewport_frame();
    if (zoom_frame_pending()) {
      observe(MetricId::ZoomAnimFrame, ms);
      zoom_frame_pending() = false;
    }
  }
  ViewportScope(const ViewportScope &) = delete;
  ViewportScope &operator=(const ViewportScope &) = delete;

private:
  bool _on;
  Clock::time_point _t0{};
};

// ---------------------------------------------------------------------------
// Serialisation. Lives here (not in the transport) so both the FileSink /
// LogSink and the HTTP endpoint share exactly one implementation.
// ---------------------------------------------------------------------------
std::string snapshot_to_text(const Snapshot &s);
std::string snapshot_to_json(const Snapshot &s);

}  // namespace perf
}  // namespace base
}  // namespace pv

// ---------------------------------------------------------------------------
// Scope macros.
//   * paint_mid keeps explicit START/END because its call site has an early
//     return between them.
//   * END does NOT flush: the window roll is driven by the host
//     (perf::frame_boundary() from the top-level widget UpdateRequest filter,
//     or the daemon timer). Flushing from a paint path was the old blind spot.
// ---------------------------------------------------------------------------
#define PXV_PERF_SCOPE_VIEWPORT() \
  ::pv::base::perf::ViewportScope _pxv_perf_vp_scope_
#define PXV_PERF_PAINTMID_START() \
  const ::pv::base::perf::Clock::time_point _pxv_pm_t0 = ::pv::base::perf::Clock::now()
#define PXV_PERF_PAINTMID_END() \
  do { \
    ::pv::base::perf::observe( \
        ::pv::base::perf::MetricId::FramePaintMid, \
        static_cast<double>(::pv::base::perf::ns_since(_pxv_pm_t0)) / 1e6); \
  } while (0)

#endif  // PXV_PERF

#endif  // PXVIEW_PV_BASE_PERF_H
