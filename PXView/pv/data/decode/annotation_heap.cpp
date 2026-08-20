/*
 * PXView — Plan A: dedicated heaps for decode-thread annotation storage.
 *
 * Root cause (see devdoc/解码器性能排查报告.md): 16 decode threads all
 * allocating annotations on the shared process heap caused the main thread to
 * stall ~1s inside ntdll's heap critical section (heap lock convoy; the GUI
 * thread was sampled stuck in RtlLockHeap/RtlEnterCriticalSection). Each
 * DecoderStack now gets its own mimalloc heap and the per-row deque + frozen
 * snapshot segments allocate from it, so the decode threads no longer contend
 * with the GUI thread's process-heap allocations.
 *
 * Instead of the former Win32-only HeapCreate/HeapValidate (which left
 * Linux/macOS as a plain malloc fallback with no isolation), we use a per-stack
 * mi_heap_t from mimalloc — cross-platform, and the same allocator that
 * libsigrokdecode/ann_batch.c already uses for its per-session annotation
 * arena. mi_free routes each block back to its owning heap, so the GUI thread
 * can safely free a published snapshot even though a decode thread allocated
 * it (producer/consumer across threads).
 *
 * mi_heap_malloc returns >= MI_MAX_ALIGN_SIZE (16) aligned memory, satisfying
 * std::deque<Annotation, HeapAllocator<Annotation>> element alignment.
 *
 * The handle is exposed as void* so the header needs no mimalloc.h; the
 * implementation lives here.
 */

#include "pv/data/decode/annotation_heap.h"

#include <cstdlib>

#if !defined(__APPLE__)
#include <mimalloc.h>
#endif

namespace pv {
namespace data {
namespace decode {

void *create_annotation_heap() {
#if defined(__APPLE__)
  // macOS: mimalloc is deliberately NOT linked into the executable — its
  // global malloc-zone override crashes the embedded Python 3.14 during
  // decoder import (libsystem malloc dispatches into the mimalloc zone and
  // the allocation faults). Return NULL so annotation_heap_alloc/free use
  // the plain std::malloc/std::free fallback below.
  return nullptr;
#else
  // Dedicated, thread-safe heap that never recycles to the OS until destroyed.
  // Each DecoderStack owns one, so the 16 decode threads no longer share the
  // process heap for annotation data.
  return mi_heap_new();
#endif
}

void destroy_annotation_heap(void *heap) {
  // mi_heap_destroy bulk-frees whatever remains. The AnnotationHeapPtr
  // refcount guarantees all RowData / AnnotationSegment / published snapshot
  // users are gone before this runs (same contract as the old HeapDestroy), so
  // no thread is touching the heap here.
  if (heap) {
#if !defined(__APPLE__)
    mi_heap_destroy(static_cast<mi_heap_t *>(heap));
#endif
  }
}

void *annotation_heap_alloc(void *heap, std::size_t n) {
#if !defined(__APPLE__)
  if (heap)
    return mi_heap_malloc(static_cast<mi_heap_t *>(heap), n);
#endif
  // null heap -> standard C allocator (safe fallback for callers that pass a
  // null handle explicitly, e.g. perflog.h probes, or on macOS where the heap
  // is intentionally disabled).
  return std::malloc(n);
}

void annotation_heap_free(void *heap, void *p) {
  if (!p)
    return;
#if !defined(__APPLE__)
  if (heap) {
    // Route by the block's owning heap header (mimalloc reads it on the
    // pointer), thread-safe by design — handles the common case where the GUI
    // thread frees a block that a decode thread allocated on this heap.
    mi_free(p);
    return;
  }
#endif
  // Match the null-heap allocation path above.
  std::free(p);
}

} // namespace decode
} // namespace data
} // namespace pv