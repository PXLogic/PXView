#ifndef PXVIEW_PV_DATA_MMAP_ALLOCATOR_H
#define PXVIEW_PV_DATA_MMAP_ALLOCATOR_H

#include <string>
#include <cstdint>
#include <mutex>
#include <thread>
#include <atomic>
#include <QString>

namespace pv {
namespace data {

class MmapAllocator {
public:
    MmapAllocator();
    ~MmapAllocator();

    bool configure(bool use_disk_file, const QString& disk_dir, uint64_t total_bytes,
                   uint64_t block_size, uint64_t max_blocks_per_channel, int channel_num);
    void* get_block_data(int channel, uint64_t block_index, uint64_t max_blocks_per_channel, uint64_t block_size);

    // 归还指定块区间的物理页给 OS（不释放虚拟映射）。
    // - 匿名 mmap (RAM 模式)：decommit 后页读回零。
    // - 文件 mmap (磁盘模式)：decommit RAM 页 + 对文件区间 punch sparse zero hole，回收磁盘空间。
    // 调用方需保证该块已不再被读（lbp 已置 NULL）。
    bool decommit_block(void* ptr, uint64_t size);

    // 由 mmap 地址反推绝对槽位序号 abs_slot = (ptr - base) / block_size。
    // 用于 LogicSnapshot 的 written 位图清位。失败（ptr 不在 mmap 区间）返回 false。
    bool block_absolute_slot(void* ptr, uint64_t block_size, uint64_t& slot) const;

    void clear();

    bool is_mmap_address(void* ptr) const {
        if (!_base_ptr) return false;
        return (uint8_t*)ptr >= (uint8_t*)_base_ptr &&
               (uint8_t*)ptr < ((uint8_t*)_base_ptr + _total_bytes);
    }

    uint64_t get_total_bytes() const { return _total_bytes; }

    void wait_prefault_initial_blocks(uint64_t block_count);
    void notify_writer_block_seq(uint64_t block_seq);

    // 停止 prefault 后台线程（用于 copy_from 等场景，防止竞态写零覆盖已复制的数据）。
    // 线程 join 后返回，保证调用方可以安全地写入 mmap 区域。
    void stop_prefault();

    void set_loop_mode(bool is_loop);

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

    // Background rolling prefault (spec: fix-mmap-prefault-decommit-multichannel)
    // 多 channel 分区布局：[ch0: 0..N][ch1: N..2N]...[chK-1: (K-1)N..KN]
    // prefault 按 block_seq 推进，每个 seq 同时 prefault 所有 channel 的对应 block。
    std::thread _prefault_thread;
    std::atomic<bool> _prefault_running;
    std::atomic<uint64_t> _writer_block_seq;
    std::atomic<uint64_t> _prefault_block_seq;
    std::atomic<uint64_t> _decommitted_block_seq;
    std::atomic<bool> _is_loop_mode;

    // Per-block-seq layout parameters（由 configure() 写入）
    uint64_t _block_size;
    uint64_t _max_blocks_per_channel;
    int _channel_num;

    static constexpr uint64_t PREFAULT_AHEAD_BLOCKS = 16;
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
    static constexpr uint64_t PREFAULT_PAGE_SIZE = 16384; // Apple Silicon page size
#else
    static constexpr uint64_t PREFAULT_PAGE_SIZE = 4096;   // Linux / Windows / Intel macOS
#endif
    static constexpr uint64_t TRAILING_DECHECK_BEHIND_BLOCKS = 16;

    void prefault_worker();
    void start_prefault();
    // stop_prefault() 已提升为 public，供 copy_from 等外部调用方使用
    void decommit_range(uint64_t start_bytes, uint64_t end_bytes);
    // 遍历所有 channel，对 block_seq 对应的 block 调用 decommit_range。
    void decommit_block_seq_all_channels(uint64_t block_seq);
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_MMAP_ALLOCATOR_H
