// PXView perf instrumentation — registry + sinks implementation.
// See pv/base/perf.h for the design rules. Compiled into pxview-core.
//
// This TU is deliberately Qt-free (std::string / std::chrono / std::mutex) so
// the unit test can link it without a QApplication.

#include "pv/base/perf.h"

#ifdef PXV_PERF

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pv {
namespace base {
namespace perf {
namespace {

// ---------------------------------------------------------------------------
// Metric table. ORDER MUST MATCH enum class MetricId — the index is the
// storage slot, which is what makes a sample O(1).
// ---------------------------------------------------------------------------
constexpr MetricDef kMetrics[] = {
    {"frame.viewport", "ms", Kind::Timer, Agg::Avg,
     "whole-viewport paint (all layers)"},
    {"frame.paint_mid", "ms", Kind::Timer, Agg::Avg,
     "decode-track painting only"},
    {"frame.interval", "ms", Kind::Timer, Agg::Avg,
     "wall time between delivered frames (continuous frames only)"},
    {"frame.paint_calls", "count", Kind::Counter, Agg::Sum,
     "frames delivered in this window"},
    {"frame.dense_rows", "count", Kind::Counter, Agg::Sum,
     "dense rows drawn"},
    {"frame.mid_rows", "count", Kind::Counter, Agg::Sum,
     "mid (annotation-block) rows drawn"},
    {"frame.ann_in_range", "ann", Kind::Counter, Agg::Avg,
     "annotations inside the visible range"},
    {"vrange.snapshot", "ms", Kind::Timer, Agg::Avg,
     "RowDataSnapshot::get_visible_range"},
    {"vrange.live", "ms", Kind::Timer, Agg::Avg,
     "RowData::get_visible_range (live path)"},
    {"decode.publish", "count", Kind::Counter, Agg::Sum,
     "snapshot publishes this window"},
    {"decode.max_publish_delta", "ann", Kind::Gauge, Agg::Max,
     "largest single publish delta"},
    {"decode.batch_flushes", "count", Kind::Counter, Agg::Sum,
     "engine -> host batch callbacks"},
    {"decode.batch_ann", "ann", Kind::Counter, Agg::Sum,
     "annotations delivered in batches"},
    {"decode.batch_max", "ann", Kind::Gauge, Agg::Max,
     "largest batch size"},
    {"copy.staging_calls", "count", Kind::Counter, Agg::Sum,
     "whole-payload copies on the capture staging path"},
    {"copy.staging_bytes", "bytes", Kind::Counter, Agg::Sum,
     "bytes copied on the capture staging path"},
    {"copy.export_calls", "count", Kind::Counter, Agg::Sum,
     "whole-payload copies on the export path"},
    {"copy.export_bytes", "bytes", Kind::Counter, Agg::Sum,
     "bytes copied on the export path"},
    {"loop.event_lag_max", "ms", Kind::Gauge, Agg::Max,
     "max heartbeat overshoot — the GUI freeze duration"},
    {"loop.cpu_util_max", "cores", Kind::Gauge, Agg::Max,
     "max process CPU utilisation per tick (1.0 = one core)"},
    {"repaint.progress", "count", Kind::Counter, Agg::Sum,
     "Viewport::on_progress_timer"},
    {"repaint.viewport", "count", Kind::Counter, Agg::Sum,
     "View::viewport_update() (full)"},
    {"repaint.decode_only", "count", Kind::Counter, Agg::Sum,
     "View::viewport_update_decode_only()"},
    {"repaint.data_updated", "count", Kind::Counter, Agg::Sum,
     "ViewDataSync::data_updated()"},
    {"repaint.update_direct", "count", Kind::Counter, Agg::Sum,
     "Viewport::update(int) — any direct call"},
    {"repaint.delayed", "count", Kind::Counter, Agg::Sum,
     "View delayed-timer drains"},
    {"repaint.delayed_full", "count", Kind::Counter, Agg::Sum,
     "  ... drained as a full viewport_update()"},
    {"repaint.delayed_decode_only", "count", Kind::Counter, Agg::Sum,
     "  ... drained as a decode-only update"},
    {"zoom.gestures", "count", Kind::Counter, Agg::Sum, "wheel zoom gestures"},
    {"zoom.ticks", "count", Kind::Counter, Agg::Sum, "zoom animation frames"},
    {"zoom.anim_frame", "ms", Kind::Timer, Agg::Avg,
     "whole-frame time while a zoom animation is running"},
    // Agg::Last, NOT Avg: the gauge is sampled on every heartbeat (~10ms) but
    // frames arrive in bursts, so a window average is dominated by the idle
    // samples and reads ~0 even while frames are flowing (measured: avg=0,
    // max=82). A frame RATE wants the most recent sample; "avg" and "max"
    // stay available in the table for the windowed view.
    {"frame.fps", "fps", Kind::Gauge, Agg::Last,
     "whole-app delivered frames per second (sliding 1s, latest sample)"},
    {"frame.viewport_fps", "fps", Kind::Gauge, Agg::Last,
     "viewport repaints per second (sliding 1s, latest sample)"},
};

static_assert(sizeof(kMetrics) / sizeof(kMetrics[0]) ==
                  static_cast<std::size_t>(MetricId::Count_),
              "kMetrics must have exactly one entry per MetricId");

// ---------------------------------------------------------------------------
// Storage.
// ---------------------------------------------------------------------------
struct Accum {
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> sum{0};  // Counter: raw sum. Timer: nanoseconds.
  std::atomic<double> vmax{0.0};
  std::atomic<double> vmin{std::numeric_limits<double>::max()};
  std::atomic<double> last{0.0};
};

// Function-local static: initialised on first use, so there is no static
// initialisation order problem across the three libraries that use this.
Accum *accum_table() noexcept {
  static Accum table[static_cast<std::size_t>(MetricId::Count_)];
  return table;
}

inline std::size_t idx(MetricId id) noexcept {
  return static_cast<std::size_t>(id);
}

inline Accum &accum(MetricId id) noexcept {
  return accum_table()[idx(id)];
}

inline void atomic_max_d(std::atomic<double> &a, double v) noexcept {
  double cur = a.load(std::memory_order_relaxed);
  while (v > cur &&
         !a.compare_exchange_weak(cur, v, std::memory_order_relaxed,
                                  std::memory_order_relaxed)) {
  }
}

inline void atomic_min_d(std::atomic<double> &a, double v) noexcept {
  double cur = a.load(std::memory_order_relaxed);
  while (v < cur &&
         !a.compare_exchange_weak(cur, v, std::memory_order_relaxed,
                                  std::memory_order_relaxed)) {
  }
}

// ---------------------------------------------------------------------------
// Named metrics (open key set — decoder row titles, main-thread op names).
// Sampled per-frame / per-operation, never per-item, so a mutex is fine.
// ---------------------------------------------------------------------------
struct NamedAccum {
  std::uint64_t calls = 0;
  double total = 0.0;
  double max = 0.0;
  double aux = 0.0;
  std::uint64_t count = 0;
};

struct NamedTables {
  std::mutex mu;
  std::unordered_map<std::string, NamedAccum> rows;
  std::unordered_map<std::string, NamedAccum> ops;
};

NamedTables &named() noexcept {
  static NamedTables t;
  return t;
}

// ---------------------------------------------------------------------------
// Runtime level / knobs.
// ---------------------------------------------------------------------------
std::atomic<std::uint8_t> g_level{static_cast<std::uint8_t>(Level::Basic)};
std::atomic<int> g_flush_interval_ms{2000};
std::atomic<int> g_heartbeat_interval_ms{100};
std::atomic<int> g_frame_idle_ms{250};

// ---------------------------------------------------------------------------
// Frame / heartbeat driver state (single-threaded: GUI thread or daemon loop).
// ---------------------------------------------------------------------------
struct DriverState {
  Clock::time_point frame_last{};
  Clock::time_point window_start{};
  int frames_in_window = 0;
  Clock::time_point hb_last{};
};

DriverState &driver() noexcept {
  static DriverState s;
  return s;
}

inline double ms_between(Clock::time_point a, Clock::time_point b) noexcept {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

inline bool unset(Clock::time_point t) noexcept {
  return t.time_since_epoch().count() == 0;
}

// Frames within the last second, as a ring of timestamps. Deliberately NOT
// tied to the summary window: the summary window rolls every ~2s, so a
// "calls / window_s" FPS would collapse to 0 right after every roll. A
// sliding 1s window gives a stable "current FPS" and drains to 0 by itself
// when frames stop (heartbeat() re-evaluates it).
struct SlidingRate {
  static constexpr int kCap = 512;
  Clock::time_point ts[kCap];
  int head = 0;
  int n = 0;

  void note(Clock::time_point now) noexcept {
    ts[head] = now;
    head = (head + 1) % kCap;
    if (n < kCap)
      ++n;
  }

  double fps(Clock::time_point now) const noexcept {
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
      if (now - ts[i] <= std::chrono::seconds(1))
        ++cnt;
    }
    return static_cast<double>(cnt);
  }
};

SlidingRate &delivered_rate() noexcept {
  static SlidingRate r;
  return r;
}
SlidingRate &viewport_rate() noexcept {
  static SlidingRate r;
  return r;
}

// ---------------------------------------------------------------------------
// Sinks.
// ---------------------------------------------------------------------------
struct SinkList {
  std::mutex mu;
  std::vector<IPerfSink *> sinks;
};

SinkList &sink_list() noexcept {
  static SinkList s;
  return s;
}

// ---------------------------------------------------------------------------
// JSON helpers (hand-rolled so perf core needs no JSON library).
// ---------------------------------------------------------------------------
void json_escape(std::string &out, const std::string &in) {
  for (char c : in) {
    switch (c) {
    case '"':  out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n";  break;
    case '\r': out += "\\r";  break;
    case '\t': out += "\\t";  break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x",
                      static_cast<unsigned>(static_cast<unsigned char>(c)));
        out += buf;
      } else {
        out += c;
      }
    }
  }
}

void json_num(std::string &out, double v) {
  char buf[48];
  // %.6g keeps the payload small; metrics are diagnostics, not exact values.
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  out += buf;
}

const char *agg_name(Agg a) noexcept {
  switch (a) {
  case Agg::Sum:  return "sum";
  case Agg::Avg:  return "avg";
  case Agg::Max:  return "max";
  case Agg::Min:  return "min";
  case Agg::Last: return "last";
  }
  return "sum";
}

const char *kind_name(Kind k) noexcept {
  switch (k) {
  case Kind::Counter: return "counter";
  case Kind::Gauge:   return "gauge";
  case Kind::Timer:   return "timer";
  }
  return "counter";
}

}  // namespace

// ---------------------------------------------------------------------------
// Public: table access.
// ---------------------------------------------------------------------------
std::size_t metric_count() noexcept {
  return static_cast<std::size_t>(MetricId::Count_);
}

const MetricDef &metric_def(MetricId id) noexcept {
  return kMetrics[idx(id)];
}

MetricId metric_by_name(const char *name) noexcept {
  if (!name)
    return MetricId::Count_;
  for (std::size_t i = 0; i < metric_count(); ++i) {
    if (std::strcmp(kMetrics[i].name, name) == 0)
      return static_cast<MetricId>(i);
  }
  return MetricId::Count_;
}

// ---------------------------------------------------------------------------
// Public: level.
// ---------------------------------------------------------------------------
Level level() noexcept {
  return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

void set_level(Level l) noexcept {
  g_level.store(static_cast<std::uint8_t>(l), std::memory_order_relaxed);
}

bool enabled() noexcept {
  return level() != Level::Off;
}

void init_from_env() noexcept {
  const char *v = std::getenv("PXVIEW_PERF_LEVEL");
  if (!v)
    return;
  if (std::strcmp(v, "off") == 0)
    set_level(Level::Off);
  else if (std::strcmp(v, "verbose") == 0)
    set_level(Level::Verbose);
  else if (std::strcmp(v, "basic") == 0)
    set_level(Level::Basic);
}

void set_flush_interval_ms(int ms) noexcept {
  g_flush_interval_ms.store(ms > 0 ? ms : 1, std::memory_order_relaxed);
}

void set_heartbeat_interval_ms(int ms) noexcept {
  g_heartbeat_interval_ms.store(ms > 0 ? ms : 1, std::memory_order_relaxed);
}

void set_frame_idle_ms(int ms) noexcept {
  g_frame_idle_ms.store(ms > 0 ? ms : 1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Public: hot-path sampling.
// ---------------------------------------------------------------------------
void add(MetricId id, std::uint64_t n) noexcept {
  if (!enabled())
    return;
  Accum &a = accum(id);
  const double d = static_cast<double>(n);
  a.calls.fetch_add(1, std::memory_order_relaxed);
  a.sum.fetch_add(n, std::memory_order_relaxed);
  atomic_max_d(a.vmax, d);
  atomic_min_d(a.vmin, d);
  a.last.store(d, std::memory_order_relaxed);
}

void set_gauge(MetricId id, double v) noexcept {
  if (!enabled())
    return;
  Accum &a = accum(id);
  a.calls.fetch_add(1, std::memory_order_relaxed);
  atomic_max_d(a.vmax, v);
  atomic_min_d(a.vmin, v);
  a.last.store(v, std::memory_order_relaxed);
}

void observe(MetricId id, double ms) noexcept {
  if (!enabled())
    return;
  Accum &a = accum(id);
  const std::uint64_t ns =
      static_cast<std::uint64_t>(ms * 1e6 + 0.5);  // NOLINT: bounded, >= 0
  a.calls.fetch_add(1, std::memory_order_relaxed);
  a.sum.fetch_add(ns, std::memory_order_relaxed);
  atomic_max_d(a.vmax, ms);
  atomic_min_d(a.vmin, ms);
  a.last.store(ms, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Public: named metrics.
// ---------------------------------------------------------------------------
void track_row(const char *name, double total_ms, double aux_ms,
               std::uint64_t count) noexcept {
  if (!name || !enabled())
    return;
  NamedTables &t = named();
  std::lock_guard<std::mutex> lk(t.mu);
  NamedAccum &n = t.rows[name];
  n.calls += 1;
  n.total += total_ms;
  n.aux += aux_ms;
  n.count += count;
  if (total_ms > n.max)
    n.max = total_ms;
}

void op_max(const char *name, double ms) noexcept {
  if (!name || !enabled())
    return;
  NamedTables &t = named();
  std::lock_guard<std::mutex> lk(t.mu);
  NamedAccum &n = t.ops[name];
  n.calls += 1;
  n.total += ms;
  if (ms > n.max)
    n.max = ms;
}

// ---------------------------------------------------------------------------
// Public: scoped helpers.
// ---------------------------------------------------------------------------
double ScopedTimer::ms_since(Clock::time_point t0) noexcept {
  return ms_between(t0, Clock::now());
}

std::uint64_t ns_since(Clock::time_point t0) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0)
          .count());
}

TrackRowScope::~TrackRowScope() noexcept {
  if (!_on)
    return;
  const double total_ms = ms_between(_t0, Clock::now());
  track_row(_name, total_ms, static_cast<double>(_aux_ns) / 1e6, _count);
}

// ---------------------------------------------------------------------------
// Public: snapshot.
// ---------------------------------------------------------------------------
Snapshot take_snapshot(bool reset) noexcept {
  Snapshot s;
  DriverState &d = driver();
  if (!unset(d.window_start))
    s.window_s = std::chrono::duration<double>(Clock::now() - d.window_start).count();

  const std::size_t n = metric_count();
  s.metrics.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    Accum &a = accum_table()[i];
    const std::uint64_t calls = a.calls.load(std::memory_order_relaxed);
    const std::uint64_t sum = a.sum.load(std::memory_order_relaxed);
    const double vmax = a.vmax.load(std::memory_order_relaxed);
    const double vmin = a.vmin.load(std::memory_order_relaxed);
    const double last = a.last.load(std::memory_order_relaxed);

    if (reset) {
      a.calls.store(0, std::memory_order_relaxed);
      a.sum.store(0, std::memory_order_relaxed);
      a.vmax.store(0.0, std::memory_order_relaxed);
      a.vmin.store(std::numeric_limits<double>::max(), std::memory_order_relaxed);
      a.last.store(0.0, std::memory_order_relaxed);
    }

    if (calls == 0)
      continue;  // nothing happened in this window — keep the payload honest

    const MetricDef &def = kMetrics[i];
    const bool is_timer = def.kind == Kind::Timer;
    const double sum_native = is_timer ? static_cast<double>(sum) / 1e6
                                       : static_cast<double>(sum);
    const double avg = sum_native / static_cast<double>(calls);

    double value = sum_native;
    switch (def.agg) {
    case Agg::Sum:  value = sum_native; break;
    case Agg::Avg:  value = avg;        break;
    case Agg::Max:  value = vmax;       break;
    case Agg::Min:  value = (vmin == std::numeric_limits<double>::max()) ? 0.0 : vmin; break;
    case Agg::Last: value = last;       break;
    }

    MetricSample ms;
    ms.id = static_cast<MetricId>(i);
    ms.calls = calls;
    ms.total = sum_native;
    ms.max = vmax;
    ms.min = (vmin == std::numeric_limits<double>::max()) ? 0.0 : vmin;
    ms.last = last;
    ms.value = value;
    s.metrics.push_back(ms);
  }

  {
    NamedTables &t = named();
    std::lock_guard<std::mutex> lk(t.mu);
    s.rows.reserve(t.rows.size());
    for (const auto &kv : t.rows) {
      NamedSample ns;
      ns.name = kv.first;
      ns.calls = kv.second.calls;
      ns.total = kv.second.total;
      ns.max = kv.second.max;
      ns.aux = kv.second.aux;
      ns.count = kv.second.count;
      s.rows.push_back(std::move(ns));
    }
    s.ops.reserve(t.ops.size());
    for (const auto &kv : t.ops) {
      NamedSample ns;
      ns.name = kv.first;
      ns.calls = kv.second.calls;
      ns.total = kv.second.total;
      ns.max = kv.second.max;
      s.ops.push_back(std::move(ns));
    }
    if (reset) {
      t.rows.clear();
      t.ops.clear();
    }
  }

  std::sort(s.rows.begin(), s.rows.end(),
            [](const NamedSample &a, const NamedSample &b) {
              return a.total > b.total;  // hottest first
            });

  if (reset) {
    d.frames_in_window = 0;
    d.window_start = Clock::now();
  }
  return s;
}

// ---------------------------------------------------------------------------
// Public: sinks + flush.
// ---------------------------------------------------------------------------
IPerfSink::~IPerfSink() = default;

void add_sink(IPerfSink *sink) noexcept {
  if (!sink)
    return;
  SinkList &l = sink_list();
  std::lock_guard<std::mutex> lk(l.mu);
  if (std::find(l.sinks.begin(), l.sinks.end(), sink) == l.sinks.end())
    l.sinks.push_back(sink);
}

void remove_sink(IPerfSink *sink) noexcept {
  SinkList &l = sink_list();
  std::lock_guard<std::mutex> lk(l.mu);
  l.sinks.erase(std::remove(l.sinks.begin(), l.sinks.end(), sink), l.sinks.end());
}

void flush_window() noexcept {
  if (!enabled())
    return;
  Snapshot s = take_snapshot(true);
  if (s.metrics.empty() && s.rows.empty() && s.ops.empty())
    return;
  SinkList &l = sink_list();
  std::lock_guard<std::mutex> lk(l.mu);
  for (IPerfSink *sink : l.sinks)
    sink->on_window(s);
}

// ---------------------------------------------------------------------------
// Public: drivers.
// ---------------------------------------------------------------------------
void frame_boundary() noexcept {
  if (!enabled())
    return;
  DriverState &d = driver();
  const Clock::time_point now = Clock::now();

  if (unset(d.window_start))
    d.window_start = now;

  if (!unset(d.frame_last)) {
    const double dt_ms = ms_between(d.frame_last, now);
    // A gap longer than the idle threshold means the UI was idle, not that a
    // frame was dropped — recording it would poison the frame-interval stats.
    if (dt_ms <= static_cast<double>(g_frame_idle_ms.load(std::memory_order_relaxed)))
      observe(MetricId::FrameInterval, dt_ms);
  }
  d.frame_last = now;

  delivered_rate().note(now);
  add(MetricId::PaintCalls, 1);
  ++d.frames_in_window;

  const double since_window =
      std::chrono::duration<double>(now - d.window_start).count();
  if (d.frames_in_window >= 120 ||
      since_window >= static_cast<double>(g_flush_interval_ms.load(std::memory_order_relaxed)) / 1000.0) {
    flush_window();
  }
}

void note_viewport_frame() noexcept {
  if (!enabled())
    return;
  viewport_rate().note(Clock::now());
}

bool &zoom_frame_pending() noexcept {
  static bool pending = false;
  return pending;
}

void heartbeat() noexcept {
  if (!enabled())
    return;
  DriverState &d = driver();
  const Clock::time_point now = Clock::now();
  if (!unset(d.hb_last)) {
    const double dt_ms = ms_between(d.hb_last, now);
    const double expected =
        static_cast<double>(g_heartbeat_interval_ms.load(std::memory_order_relaxed));
    const double lag = dt_ms - expected;
    set_gauge(MetricId::EventLagMax, lag > 0.0 ? lag : 0.0);
  }
  d.hb_last = now;

  // Re-evaluate the frame rates here rather than only on frames, so they
  // drain to 0 when the UI goes idle instead of showing a stale value.
  set_gauge(MetricId::FrameFps, delivered_rate().fps(now));
  set_gauge(MetricId::FrameViewportFps, viewport_rate().fps(now));
}

// ---------------------------------------------------------------------------
// Public: serialisation.
// ---------------------------------------------------------------------------
std::string snapshot_to_text(const Snapshot &s) {
  std::string out;
  char line[512];

  std::snprintf(line, sizeof(line), "================ WINDOW (%.2fs) ================\n",
                s.window_s);
  out += line;

  for (const MetricSample &m : s.metrics) {
    const MetricDef &d = kMetrics[idx(m.id)];
    std::snprintf(line, sizeof(line),
                  "%-26s calls=%-6" PRIu64 " total=%.3f%s max=%.3f%s "
                  "min=%.3f%s avg=%.3f%s %s=%.3f%s\n",
                  d.name, m.calls, m.total, d.unit, m.max, d.unit, m.min,
                  d.unit, m.calls ? m.total / static_cast<double>(m.calls) : 0.0,
                  d.unit, agg_name(d.agg), m.value, d.unit);
    out += line;
  }

  if (!s.rows.empty()) {
    std::snprintf(line, sizeof(line), "TRACK ROWS (%zu):\n", s.rows.size());
    out += line;
    for (const NamedSample &r : s.rows) {
      std::snprintf(line, sizeof(line),
                    "  %-28s calls=%-6" PRIu64 " total=%.3fms max=%.3fms "
                    "avg=%.3fms | aux_avg=%.4fms count_avg=%.1f\n",
                    r.name.c_str(), r.calls, r.total, r.max,
                    r.calls ? r.total / static_cast<double>(r.calls) : 0.0,
                    r.calls ? r.aux / static_cast<double>(r.calls) : 0.0,
                    r.calls ? static_cast<double>(r.count) / static_cast<double>(r.calls) : 0.0);
      out += line;
    }
  }

  if (!s.ops.empty()) {
    out += "OP_MAX:";
    for (const NamedSample &o : s.ops) {
      std::snprintf(line, sizeof(line), " %s=%.1fms", o.name.c_str(), o.max);
      out += line;
    }
    out += "\n";
  }

  out += "\n";
  return out;
}

std::string snapshot_to_json(const Snapshot &s) {
  std::string out;
  out.reserve(2048);
  char buf[64];

  out += "{\"window_s\":";
  json_num(out, s.window_s);

  out += ",\"metrics\":[";
  bool first = true;
  for (const MetricSample &m : s.metrics) {
    const MetricDef &d = kMetrics[idx(m.id)];
    if (!first)
      out += ',';
    first = false;
    out += "{\"id\":\"";
    json_escape(out, d.name);
    out += "\",\"unit\":\"";
    json_escape(out, d.unit);
    out += "\",\"kind\":\"";
    out += kind_name(d.kind);
    out += "\",\"agg\":\"";
    out += agg_name(d.agg);
    out += "\",\"desc\":\"";
    json_escape(out, d.desc);
    out += "\",\"calls\":";
    std::snprintf(buf, sizeof(buf), "%" PRIu64, m.calls);
    out += buf;
    out += ",\"total\":";
    json_num(out, m.total);
    out += ",\"avg\":";
    json_num(out, m.calls ? m.total / static_cast<double>(m.calls) : 0.0);
    out += ",\"max\":";
    json_num(out, m.max);
    out += ",\"min\":";
    json_num(out, m.min);
    out += ",\"value\":";
    json_num(out, m.value);
    out += '}';
  }
  out += ']';

  out += ",\"rows\":[";
  first = true;
  for (const NamedSample &r : s.rows) {
    if (!first)
      out += ',';
    first = false;
    out += "{\"name\":\"";
    json_escape(out, r.name);
    out += "\",\"calls\":";
    std::snprintf(buf, sizeof(buf), "%" PRIu64, r.calls);
    out += buf;
    out += ",\"total_ms\":";
    json_num(out, r.total);
    out += ",\"max_ms\":";
    json_num(out, r.max);
    out += ",\"avg_ms\":";
    json_num(out, r.calls ? r.total / static_cast<double>(r.calls) : 0.0);
    out += ",\"aux_avg_ms\":";
    json_num(out, r.calls ? r.aux / static_cast<double>(r.calls) : 0.0);
    out += ",\"count_avg\":";
    json_num(out, r.calls ? static_cast<double>(r.count) / static_cast<double>(r.calls) : 0.0);
    out += '}';
  }
  out += ']';

  out += ",\"ops\":[";
  first = true;
  for (const NamedSample &o : s.ops) {
    if (!first)
      out += ',';
    first = false;
    out += "{\"name\":\"";
    json_escape(out, o.name);
    out += "\",\"max_ms\":";
    json_num(out, o.max);
    out += '}';
  }
  out += "]}";
  return out;
}

}  // namespace perf
}  // namespace base
}  // namespace pv

#endif  // PXV_PERF
