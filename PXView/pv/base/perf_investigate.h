#ifndef PXVIEW_PV_BASE_PERF_INVESTIGATE_H
#define PXVIEW_PV_BASE_PERF_INVESTIGATE_H

// ============================================================================
// One-off probes whose hypotheses are ALREADY RESOLVED.
//
// Parked behind their own gate (-DENABLE_PERF_INVESTIGATE=ON, which also
// implies PXV_PERF) so an ordinary instrumented build stays clean, but the
// investigation can still be re-run deliberately. Nothing here is a metric.
//
// Contents: the two Windows heap probes from the "GUI convoy" hunt (P3-D8 in
// the old numbering). Conclusion reached, recorded in the 2026-10 perf logs:
//   * CRT malloc / C++ operator new / Qt qMalloc / glib g_malloc all land on
//     ONE heap (the process heap) => one lock shared with the GUI thread.
//   * Plan A's per-stack HeapCreate heaps are genuinely separate.
//   * A decode thread's g_malloc lands on that same process heap.
// Sampling is therefore off by default; the probes only exist to re-confirm.
// ============================================================================

#ifdef PXV_PERF_INVESTIGATE

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <glib.h>

#include "pv/data/decode/annotation_heap.h"

namespace pv {
namespace base {
namespace perf {

// Which heap owns a given pointer (HeapValidate scans the process heap list).
inline HANDLE probe_heap_of_pointer(const void *p) {
  if (!p)
    return nullptr;
  DWORD n = GetProcessHeaps(0, nullptr);
  if (!n || n > 64)
    return nullptr;
  HANDLE heaps[64];
  n = GetProcessHeaps(n, heaps);
  for (DWORD i = 0; i < n; i++) {
    if (HeapValidate(heaps[i], 0, p))
      return heaps[i];
  }
  return nullptr;
}

// Recorded from a decode thread: which heap a g_malloc'd block lands on there.
// Equal to the main thread's CRT heap == the decode threads share the GUI
// thread's heap lock (the convoy source). Relaxed write; diagnostic only.
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

// Snapshot of the heap topology, for a probe sink to print.
struct HeapTopology {
  HANDLE process = nullptr;
  HANDLE crt_malloc = nullptr;
  HANDLE cxx_new = nullptr;
  HANDLE glib_malloc = nullptr;
  HANDLE plan_a = nullptr;
  HANDLE decode_thread = nullptr;
};

inline HeapTopology sample_heap_topology() {
  HeapTopology t;
  t.process = GetProcessHeap();
  void *p_crt = ::malloc(64);
  void *p_cxx = ::operator new(64);
  void *p_glib = g_malloc(64);
  t.crt_malloc = probe_heap_of_pointer(p_crt);
  t.cxx_new = probe_heap_of_pointer(p_cxx);
  t.glib_malloc = probe_heap_of_pointer(p_glib);
  void *heap_a = pv::data::decode::create_annotation_heap();
  if (heap_a) {
    void *pa = pv::data::decode::annotation_heap_alloc(heap_a, 64);
    if (pa)
      t.plan_a = probe_heap_of_pointer(pa);
    pv::data::decode::annotation_heap_free(heap_a, pa);
    pv::data::decode::destroy_annotation_heap(heap_a);
  }
  ::free(p_crt);
  ::operator delete(p_cxx);
  g_free(p_glib);
  t.decode_thread = g_decode_thread_heap;
  return t;
}

}  // namespace perf
}  // namespace base
}  // namespace pv

#else  // !PXV_PERF_INVESTIGATE

namespace pv {
namespace base {
namespace perf {
inline void record_decode_thread_heap() {}
}  // namespace perf
}  // namespace base
}  // namespace pv

#endif  // PXV_PERF_INVESTIGATE

#endif  // PXVIEW_PV_BASE_PERF_INVESTIGATE_H
