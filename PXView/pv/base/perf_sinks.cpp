// Built-in perf sinks. See pv/base/perf_sinks.h.
// Compiled into pxview-core.

#include "pv/base/perf_sinks.h"

#ifdef PXV_PERF

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "pv/base/log.h"  // pxv_log / pxv_info — routes LogSink into PXView.log

namespace pv {
namespace base {
namespace perf {
namespace {

// Written with a fixed name and a hard cap; never grows across sessions.
std::string g_owned_paths[4];  // keeps FileSink/JsonSink paths alive

}  // namespace

std::string default_perf_log_path() {
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
  if (ec)
    return "pxview_perf.log";
  return (dir / "pxview_perf.log").string();
}

// ---------------------------------------------------------------------------
// FileSink
// ---------------------------------------------------------------------------
FileSink::FileSink(std::string path, std::size_t max_bytes)
    : _path(std::move(path)), _max_bytes(max_bytes ? max_bytes : 1) {}

void FileSink::on_window(const Snapshot &s) {
  std::lock_guard<std::mutex> lk(_mu);

  // Rotate before writing so a single window can never blow past the cap.
  if (_started && _written >= _max_bytes) {
    const std::string rolled = _path + ".1";
    std::remove(rolled.c_str());
    std::rename(_path.c_str(), rolled.c_str());
    _started = false;
    _written = 0;
  }

  const char *mode = _started ? "a" : "w";
  std::FILE *f = std::fopen(_path.c_str(), mode);
  if (!f)
    return;
  _started = true;

  const std::string text = snapshot_to_text(s);
  const std::size_t n = std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  _written += n;
}

// ---------------------------------------------------------------------------
// LogSink
// ---------------------------------------------------------------------------
void LogSink::on_window(const Snapshot &s) {
  // Split into lines: xlog prefixes each call, so one call per line keeps the
  // existing LogDock readable (and level-filterable).
  const std::string text = snapshot_to_text(s);
  std::string line;
  for (char c : text) {
    if (c == '\n') {
      if (!line.empty()) {
        pxv_info("perf %s", line.c_str());
        line.clear();
      }
    } else {
      line += c;
    }
  }
  if (!line.empty())
    pxv_info("perf %s", line.c_str());
}

// ---------------------------------------------------------------------------
// JsonSink
// ---------------------------------------------------------------------------
JsonSink::JsonSink(std::string path) : _path(std::move(path)) {}

void JsonSink::on_window(const Snapshot &s) {
  std::lock_guard<std::mutex> lk(_mu);
  const char *mode = _started ? "a" : "w";
  std::FILE *f = std::fopen(_path.c_str(), mode);
  if (!f)
    return;
  _started = true;
  const std::string json = snapshot_to_json(s);
  std::fwrite(json.data(), 1, json.size(), f);
  std::fputc('\n', f);
  std::fclose(f);
}

// ---------------------------------------------------------------------------
// install_default_sinks
// ---------------------------------------------------------------------------
int install_default_sinks() {
  int n = 0;

  g_owned_paths[0] = default_perf_log_path();
  static FileSink file_sink(g_owned_paths[0]);
  add_sink(&file_sink);
  ++n;

  if (const char *json_path = std::getenv("PXVIEW_PERF_JSON")) {
    if (*json_path) {
      g_owned_paths[1] = json_path;
      static JsonSink json_sink(g_owned_paths[1]);
      add_sink(&json_sink);
      ++n;
    }
  }

  if (const char *mirror = std::getenv("PXVIEW_PERF_LOG")) {
    if (mirror && std::string(mirror) == "1") {
      static LogSink log_sink;
      add_sink(&log_sink);
      ++n;
    }
  }

  return n;
}

}  // namespace perf
}  // namespace base
}  // namespace pv

#endif  // PXV_PERF
