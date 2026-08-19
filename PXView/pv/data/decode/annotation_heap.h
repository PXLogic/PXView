/*
 * PXView — Plan A: dedicated heaps for decode-thread annotation storage.
 *
 * Root cause (see devdoc/解码器性能排查报告.md): 16 decode threads all
 * allocating annotations on the shared process heap caused the main thread to
 * stall ~1s inside ntdll's heap critical section (heap lock convoy; the GUI
 * thread was sampled stuck in RtlLockHeap/RtlEnterCriticalSection). Each
 * DecoderStack now gets its own growable heap and the per-row deque + frozen
 * snapshot segments allocate from it, so the decode threads no longer contend
 * with the GUI thread's process-heap allocations.
 *
 * The handle is exposed as void* so this header needs no <windows.h>; the
 * implementation lives in annotation_heap.cpp.
 */

#ifndef PXVIEW_PV_DATA_DECODE_ANNOTATION_HEAP_H
#define PXVIEW_PV_DATA_DECODE_ANNOTATION_HEAP_H

#include <cstddef>
#include <memory>
#include <new>

namespace pv {
namespace data {
namespace decode {

void *create_annotation_heap();
void destroy_annotation_heap(void *heap);
void *annotation_heap_alloc(void *heap, std::size_t n);
void annotation_heap_free(void *heap, void *p);

// Reference-counted heap handle: keeps the underlying HANDLE alive as long as
// any RowData / AnnotationSegment / published snapshot that allocated from it
// still exists (the GUI render path can hold a snapshot after the stack is
// destroyed). The raw HANDLE is obtained via get().
using AnnotationHeapPtr = std::shared_ptr<void>;
inline AnnotationHeapPtr make_annotation_heap() {
  return AnnotationHeapPtr(create_annotation_heap(),
                           [](void *h) { destroy_annotation_heap(h); });
}

// Minimal C++ allocator backed by a HANDLE heap. null heap -> process heap
// (safe fallback on Windows). On non-Windows platforms the heap pointer is
// always null; annotation_heap_alloc/free fall back to malloc/free so the
// allocator is fully functional (just not isolated from the process heap).
// Copyable; used as the deque allocator for annotation data.
// HeapAlloc/HeapFree are serialized (no HEAP_NO_SERIALIZE), so a snapshot
// freed on the GUI thread while the decode thread keeps allocating on the same
// per-stack heap is safe.
template <typename T>
class HeapAllocator {
public:
  using value_type = T;
  void *heap = nullptr;

  HeapAllocator() = default;
  explicit HeapAllocator(void *h) : heap(h) {}
  template <typename U>
  HeapAllocator(const HeapAllocator<U> &o) : heap(o.heap) {}

  T *allocate(std::size_t n) {
    if (n == 0)
      return nullptr;
    void *p = annotation_heap_alloc(heap, n * sizeof(T));
    if (!p)
      throw std::bad_alloc();
    return static_cast<T *>(p);
  }
  void deallocate(T *p, std::size_t) noexcept {
    if (p)
      annotation_heap_free(heap, p);
  }
  template <typename U>
  bool operator==(const HeapAllocator<U> &o) const {
    return heap == o.heap;
  }
  template <typename U>
  bool operator!=(const HeapAllocator<U> &o) const {
    return !(*this == o);
  }
};

} // namespace decode
} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_DECODE_ANNOTATION_HEAP_H
