/*
 * Annotation Memory Pool
 * 
 * A thread-safe, chunk-based memory pool for Annotation objects.
 * Eliminates heap fragmentation caused by millions of small allocations.
 * 
 * Design:
 *   - Allocates large chunks (CHUNK_SIZE objects at a time) from the system.
 *   - Maintains a free-list for O(1) alloc/dealloc.
 *   - When the free-list is empty, a new chunk is allocated.
 *   - Chunks are never returned to the OS until the pool is destroyed,
 *     but their memory is immediately reusable for new Annotation objects.
 */

#ifndef PXVIEW_PV_DATA_DECODE_ANNOTATION_POOL_H
#define PXVIEW_PV_DATA_DECODE_ANNOTATION_POOL_H

#include <cstdlib>
#include <cstddef>
#include <mutex>
#include <vector>

namespace pv {
namespace data {
namespace decode {

class AnnotationPool {
public:
    static constexpr size_t CHUNK_SIZE = 100000; // objects per chunk

    static AnnotationPool& instance() {
        static AnnotationPool pool;
        return pool;
    }

    void* allocate(size_t obj_size) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_obj_size == 0) {
            _obj_size = obj_size;
        }

        if (_free_list.empty()) {
            grow();
        }

        void* p = _free_list.back();
        _free_list.pop_back();
        _alloc_count++;
        return p;
    }

    void deallocate(void* p) {
        if (p == nullptr) return;
        std::lock_guard<std::mutex> lock(_mutex);
        _free_list.push_back(p);
        _alloc_count--;
    }

    // Statistics for debugging
    size_t alloc_count() const { return _alloc_count; }
    size_t free_count() const { return _free_list.size(); }
    size_t chunk_count() const { return _chunks.size(); }
    size_t total_capacity() const { return _chunks.size() * CHUNK_SIZE; }
    size_t obj_size() const { return _obj_size; }

    // Total memory held by the pool (in bytes)
    size_t total_memory_bytes() const {
        return _chunks.size() * CHUNK_SIZE * _obj_size;
    }

private:
    AnnotationPool() : _obj_size(0), _alloc_count(0) {
        _free_list.reserve(CHUNK_SIZE * 4); // Pre-reserve free-list capacity
    }

    ~AnnotationPool() {
        for (void* chunk : _chunks) {
            std::free(chunk);
        }
    }

    AnnotationPool(const AnnotationPool&) = delete;
    AnnotationPool& operator=(const AnnotationPool&) = delete;

    void grow() {
        // Allocate a big raw chunk for CHUNK_SIZE objects
        char* chunk = static_cast<char*>(std::malloc(_obj_size * CHUNK_SIZE));
        if (!chunk) return; // OOM

        _chunks.push_back(chunk);

        // Slice the chunk into individual object slots and add to free-list
        for (size_t i = 0; i < CHUNK_SIZE; i++) {
            _free_list.push_back(chunk + i * _obj_size);
        }
    }

    std::mutex _mutex;
    std::vector<void*> _free_list;   // available slots
    std::vector<void*> _chunks;      // owned raw memory
    size_t _obj_size;                // sizeof(Annotation), set on first alloc
    size_t _alloc_count;             // currently outstanding allocations
};

} // namespace decode
} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_DECODE_ANNOTATION_POOL_H
