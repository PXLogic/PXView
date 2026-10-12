#ifndef PXVIEW_PV_BASE_PERF_SINKS_H
#define PXVIEW_PV_BASE_PERF_SINKS_H

// ============================================================================
// Built-in IPerfSink implementations. Kept in a separate header from perf.h so
// the hot-path declaration stays free of filesystem / logging concerns, and so
// a unit test can link perf.cpp alone without dragging these in.
//
// Layer: Core (pv/base, compiled into pxview-core). The LogSink is the only
// one that touches Qt (via pv/base/log.h -> xlog); the others are Qt-free.
// ============================================================================

#include "pv/base/perf.h"

#ifdef PXV_PERF

#include <cstddef>
#include <mutex>
#include <string>

namespace pv {
namespace base {
namespace perf {

// Rotating text log. Truncates on first write of the process, then rotates to
// "<path>.1" once the cap is exceeded, so the file can never grow unbounded
// (the previous zoom-trace file had reached 3.95 MB and kept climbing).
class FileSink : public IPerfSink {
public:
  explicit FileSink(std::string path, std::size_t max_bytes = 4u * 1024u * 1024u);
  const char *name() const override { return "file"; }
  void on_window(const Snapshot &s) override;

private:
  std::mutex _mu;
  std::string _path;
  std::size_t _max_bytes;
  std::size_t _written = 0;
  bool _started = false;
};

// Routes the window through the existing xlog so perf lines land in PXView.log
// and show up in the existing LogDock (level filter, save-to-file) for free.
class LogSink : public IPerfSink {
public:
  const char *name() const override { return "log"; }
  void on_window(const Snapshot &s) override;
};

// One JSON object per window, newline-delimited (NDJSON) — machine-readable so
// CI can assert on counters instead of scraping human-readable text.
class JsonSink : public IPerfSink {
public:
  explicit JsonSink(std::string path);
  const char *name() const override { return "json"; }
  void on_window(const Snapshot &s) override;

private:
  std::mutex _mu;
  std::string _path;
  bool _started = false;
};

// Installs the default sink set for a process: a FileSink at
// <temp>/pxview_perf.log. Honours two env overrides:
//   PXVIEW_PERF_JSON=<path>  also write NDJSON to <path>
//   PXVIEW_PERF_LOG=1        also mirror into xlog / PXView.log
// Returns the number of sinks installed.
int install_default_sinks();

// Default path used by install_default_sinks() / FileSink.
std::string default_perf_log_path();

}  // namespace perf
}  // namespace base
}  // namespace pv

#endif  // PXV_PERF

#endif  // PXVIEW_PV_BASE_PERF_SINKS_H
