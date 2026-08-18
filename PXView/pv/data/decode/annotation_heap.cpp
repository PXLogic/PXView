/*
 * PXView — Plan A: dedicated heap implementation (Windows).
 *
 * One growable, serialized heap per DecoderStack for decode-thread annotation
 * storage. See annotation_heap.h for the rationale. This translation unit is
 * the only one that needs <windows.h> for the heap APIs.
 */

#include "pv/data/decode/annotation_heap.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pv {
namespace data {
namespace decode {

void *create_annotation_heap() {
#ifdef _WIN32
  // Flags 0 = serialized (thread-safe) + growable; initial 0, max 0 = grow
  // on demand. Each DecoderStack owns one, so the 16 decode threads no longer
  // share the process heap for annotation data.
  return HeapCreate(0, 0, 0);
#else
  return nullptr;
#endif
}

void destroy_annotation_heap(void *heap) {
#ifdef _WIN32
  if (heap)
    HeapDestroy(static_cast<HANDLE>(heap));
#else
  (void)heap;
#endif
}

void *annotation_heap_alloc(void *heap, std::size_t n) {
#ifdef _WIN32
  return HeapAlloc(heap ? static_cast<HANDLE>(heap) : GetProcessHeap(), 0, n);
#else
  (void)heap;
  (void)n;
  return nullptr;
#endif
}

void annotation_heap_free(void *heap, void *p) {
#ifdef _WIN32
  if (p)
    HeapFree(heap ? static_cast<HANDLE>(heap) : GetProcessHeap(), 0, p);
#else
  (void)heap;
  (void)p;
#endif
}

} // namespace decode
} // namespace data
} // namespace pv
