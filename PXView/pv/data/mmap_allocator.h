#ifndef PXVIEW_PV_DATA_MMAP_ALLOCATOR_H
#define PXVIEW_PV_DATA_MMAP_ALLOCATOR_H

#include <string>
#include <cstdint>
#include <mutex>
#include <QString>

namespace pv {
namespace data {

class MmapAllocator {
public:
    MmapAllocator();
    ~MmapAllocator();

    bool configure(bool use_disk_file, const QString& disk_dir, uint64_t total_bytes);
    void* get_block_data(int channel, uint64_t block_index, uint64_t max_blocks_per_channel, uint64_t block_size);
    void clear();

    bool is_mmap_address(void* ptr) const {
        if (!_base_ptr) return false;
        return (uint8_t*)ptr >= (uint8_t*)_base_ptr && 
               (uint8_t*)ptr < ((uint8_t*)_base_ptr + _total_bytes);
    }
    
    uint64_t get_total_bytes() const { return _total_bytes; }

private:
    void* _base_ptr;
    uint64_t _total_bytes;
    QString _file_path;
#ifdef _WIN32
    void* _hMap;
    void* _hFile;
#else
    int _fd;
#endif
    std::mutex _mutex;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_MMAP_ALLOCATOR_H
