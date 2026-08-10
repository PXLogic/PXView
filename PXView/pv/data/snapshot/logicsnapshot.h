/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */


#ifndef PXVIEW_PV_DATA_LOGICSNAPSHOT_H
#define PXVIEW_PV_DATA_LOGICSNAPSHOT_H

#include <libsigrok/libsigrok.h>
#include "pv/data/snapshot/snapshot.h"
#include "pv/data/cache/mmap_allocator.h"
#include "pv/data/cache/disk_cache_config.h"
#include "pv/base/pxvdef.h"  // GlitchFilterMode (moved here from this header)
#include <QString>
#include <utility>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#define CHANNEL_MAX_COUNT 64

// Extracted disk-cache/async-writer subsystem (cluster D). Defined in
// logicsnapshot_diskcache_writer.h/.cpp. Forward-declared here to avoid a
// circular include; LogicSnapshot holds it via unique_ptr.
class LogicSnapshotDiskCacheWriter;

namespace pv {
namespace data {

// Extracted glitch-filter subsystem (cluster C). Defined in
// logicsnapshot_glitch_filter.h/.cpp. Forward-declared here; LogicSnapshot
// holds it via unique_ptr and forwards the public methods. Note: the
// glitch-filter header includes this file (one-way) because its API references
// the nested FillRange type.
class LogicSnapshotGlitchFilter;

class LogicSnapshot : public Snapshot
{
private:
    static const uint64_t ScaleLevel = 4;
    static const uint64_t ScalePower = 6;
    static const uint64_t Scale = 1 << ScalePower;
    static const uint64_t ScaleSize = Scale / 8;
    static const uint64_t RootScalePower = ScalePower;
    static const uint64_t RootScale = 1 << RootScalePower;
    static const uint64_t LeafBlockSpace = (Scale + Scale*Scale +
            Scale*Scale*Scale + Scale*Scale*Scale*Scale) / 8;

    static const uint64_t LeafBlockPower = ScaleLevel*ScalePower;
    static const uint64_t LeafBlockSamples = 1 << LeafBlockPower;
    static const uint64_t RootNodeSamples = LeafBlockSamples*RootScale;

    static const uint64_t RootMask = ~(~0ULL << RootScalePower) << LeafBlockPower;
    static const uint64_t LeafMask = ~(~0ULL << LeafBlockPower);
    static const uint64_t LevelMask[ScaleLevel];
    static const uint64_t LevelOffset[ScaleLevel];

    static const uint64_t MSB =  (1ULL << (Scale - 1));
    static const uint64_t LSB =  (1ULL);

private:
    struct RootNode
    {
        uint64_t tog;
        uint64_t first;
        uint64_t last;
        void *lbp[Scale];
    };

    struct BlockIndex
    {
        uint64_t    root_index;
        uint64_t    lbp_index;
    };

public:
    using EdgePair = std::pair<uint64_t, bool>;

    // 持久化的滤波区间信息（apply_glitch_filter 滤除的区间），供 View 层渲染 overlay
    struct FillRange {
        uint64_t start;
        uint64_t end;
        bool level;
    };

private:
    void init_all();

public:
    LogicSnapshot();

	virtual ~LogicSnapshot();

    void free_data();

    void clear();

    void init();   

    void first_payload(const sr_datafeed_logic &logic, uint64_t total_sample_count, GSList *channels, bool able_free);

	void append_payload(const sr_datafeed_logic &logic);

    const uint8_t * get_samples(uint64_t start_sample, uint64_t& end_sample, int sig_index, void **lbp=nullptr);

    bool get_sample(uint64_t index, int sig_index);

    void capture_ended();

    void copy_from(const LogicSnapshot &src);

    bool get_display_edges(std::vector<std::pair<bool, bool>> &edges,
                           std::vector<std::pair<uint16_t, bool>> &togs,
                           uint64_t start, uint64_t end, uint16_t width,
                           uint16_t max_togs, double pixels_offset,
                           double min_length, uint16_t sig_index);

    bool get_nxt_edge(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);

    bool get_pre_edge(uint64_t &index, bool last_sample,
                      double min_length, int sig_index);

    void invert_channel(int sig_index);
    void apply_glitch_filter(int sig_index, uint32_t threshold, std::function<void(int)> progress_callback,
        GlitchFilterMode filter_mode = GlitchFilterMode::Both);
    void apply_glitch_filter_all(const std::map<int, uint32_t> &thresholds, std::function<void(int)> progress_callback,
        const std::map<int, GlitchFilterMode> &filter_modes = {});
    bool is_glitch_filtered();
    void set_glitch_filtered(bool filtered);

    // 持久化访问 apply_glitch_filter 滤除的区间列表，供 View 层渲染 overlay
    const std::vector<FillRange>& get_filtered_ranges(int sig_index) const;
    void clear_filtered_ranges();

    void set_disk_cache_config(const DiskCacheConfig &config);
    bool is_disk_cache_active();
    double get_disk_write_speed_mbps();
    size_t get_disk_write_queue_depth();
    uint64_t get_disk_total_blocks_written();
    void ensure_all_blocks_hot();

    uint64_t get_page_fault_count();
    uint64_t get_working_set_bytes();
    uint64_t get_async_queue_bytes();

    bool has_data(int sig_index);
    int get_block_num();
    uint64_t get_block_size(int block_index);
    uint8_t *get_block_buf(int block_index, int sig_index, bool &sample);
 
    bool pattern_search(int64_t start, int64_t end, int64_t& index,
                        std::map<uint16_t, QString> &pattern, bool isNext);

    inline void set_loop(bool bLoop){
        _is_loop = bLoop;
    }

    inline bool is_loop(){
        return _is_loop;
    }

    void decode_end();

    void free_decode_lpb(void *lbp);

    // P1-6 fix: Iterator reference counting, matching PulseView's
    // Segment::iterator_count_ design. When the decode thread (or any
    // reader) calls get_samples(), it increments _iterator_count via
    // begin_iteration(). Memory-optimization operations (free_data,
    // free_head_blocks) check this count and skip if > 0, preventing
    // use-after-free during concurrent memory cleanup.
    inline void begin_iteration() {
        _iterator_count.fetch_add(1, std::memory_order_acquire);
    }
    inline void end_iteration() {
        _iterator_count.fetch_sub(1, std::memory_order_release);
    }
    inline bool has_active_iterators() const {
        return _iterator_count.load(std::memory_order_acquire) > 0;
    }

    // RAII guard for iterator counting
    struct IteratorGuard {
        LogicSnapshot *_snap;
        IteratorGuard(LogicSnapshot *s) : _snap(s) {
            if (_snap) _snap->begin_iteration();
        }
        ~IteratorGuard() {
            if (_snap) _snap->end_iteration();
        }
        IteratorGuard(const IteratorGuard&) = delete;
        IteratorGuard& operator=(const IteratorGuard&) = delete;
    };

    // P1-B: Segment data iterator protocol, matching PulseView's
    // Segment::begin_sample_iteration / continue_sample_iteration /
    // end_sample_iteration design.  Provides chunk-level contiguous
    // memory access so the decode thread can batch-read samples without
    // calling get_samples() for every chunk.
    struct SegmentDataIterator {
        uint64_t current_sample = 0;   // absolute sample position
        int       ch_order = 0;        // channel order index
        uint64_t root_index = 0;       // index into _ch_data[order]
        uint64_t lbp_index = 0;        // index into RootNode.lbp[]
        uint64_t byte_offset = 0;      // byte offset within current leaf block
        const uint8_t *chunk_data = nullptr;  // pointer to current leaf block data
        uint64_t chunk_remaining = 0;  // remaining bytes in current leaf block
        bool     exhausted = false;    // all samples consumed
    };

    SegmentDataIterator* begin_sample_iteration(uint64_t start, int sig_index);
    void continue_sample_iteration(SegmentDataIterator* it, uint64_t increase);
    void end_sample_iteration(SegmentDataIterator* it);
    static inline const uint8_t* get_iterator_value(SegmentDataIterator* it) {
        return it->chunk_data + it->byte_offset;
    }
    static inline uint64_t get_iterator_valid_length(SegmentDataIterator* it) {
        return it->chunk_remaining;
    }

    inline bool is_able_free(){
        return _able_free;
    } 

    inline uint64_t get_loop_offset(){
        return _loop_offset;
    }

    static int get_block_with_sample(uint64_t index, uint64_t *out_offset);

private:
    bool get_sample_unlock(uint64_t index, int sig_index);
    bool get_sample_self(uint64_t index, int sig_index);

    bool get_nxt_edge_unlock(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);
    bool get_nxt_edge_self(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);

    bool get_pre_edge_self(uint64_t &index, bool last_sample,
                      double min_length, int sig_index);

    bool pattern_search_self(int64_t start, int64_t end, int64_t& index,
                        std::map<uint16_t, QString> &pattern, bool isNext);

    int get_ch_order(int sig_index);

    void calc_mipmap(unsigned int order, uint8_t index0, uint8_t index1, uint64_t samples, bool isEnd);

    void append_payload_impl(const sr_datafeed_logic &logic);

    /** Append raw channel-block (LA_CROSS_DATA) payload. v1.49 algorithm:
     *  bit-copy raw bytes directly into per-channel chunk tree, no
     *  deinterleave. Called by DiskCacheWriter async worker when
     *  logic.format == LA_CROSS_DATA. */
    void append_cross_payload(const sr_datafeed_logic &logic);

    bool lbp_nxt_edge(uint64_t &index, uint64_t root_index, uint64_t lbp_tog, uint8_t lbp_tog_pos,
                      bool aft_tog, uint8_t aft_pos, bool last_sample, int sig_index);

    bool block_nxt_edge(uint64_t *lbp, uint64_t &index, uint64_t block_end, bool last_sample,
                        unsigned int min_level);

    bool lbp_pre_edge(uint64_t &index, uint64_t root_index, uint64_t lbp_tog, uint8_t &lbp_tog_pos,
                      bool pre_tog, uint8_t pre_pos, bool last_sample, int sig_index);

    bool block_pre_edge(uint64_t *lbp, uint64_t &index, bool last_sample,
                        unsigned int min_level, int sig_index);

    inline uint8_t bsf_folded (uint64_t bb)
    {
        static const uint8_t lsb_64_table[64] = {
            63, 30,  3, 32, 59, 14, 11, 33,
            60, 24, 50,  9, 55, 19, 21, 34,
            61, 29,  2, 53, 51, 23, 41, 18,
            56, 28,  1, 43, 46, 27,  0, 35,
            62, 31, 58,  4,  5, 49, 54,  6,
            15, 52, 12, 40,  7, 42, 45, 16,
            25, 57, 48, 13, 10, 39,  8, 44,
            20, 47, 38, 22, 17, 37, 36, 26
        };
        unsigned int folded;
        bb ^= bb - 1;
        folded = (int) bb ^ (bb >> 32);
        return lsb_64_table[folded * 0x78291ACF >> 26];
    }

    inline uint8_t bsr32(uint32_t bb)
    {
        static const uint8_t msb_256_table[256] = {
            0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
            4, 4, 4, 4, 4, 4, 4, 4,4, 4, 4, 4,4, 4, 4, 4,
            5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
            6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
            6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
       };
       uint8_t result = 0;

       if (bb > 0xFFFF) {
          bb >>= 16;
          result += 16;
       }
       if (bb > 0xFF) {
          bb >>= 8;
          result += 8;
       }

       return (result + msb_256_table[bb]);
    }

    inline uint8_t bsr64(uint64_t bb)
    {
        const uint32_t hb = bb >> 32;
        return hb ? 32 + bsr32((uint32_t)hb) : bsr32((uint32_t)bb);
    }

    void move_first_node_to_last();

    void free_head_blocks(int count);

// B-6: Override to connect PulseView-style free_unused_memory() to
// the existing free_head_blocks() mechanism. Called by Snapshot::
// set_complete() (via _mem_optimization_requested) after capture ends.
void free_unused_memory() override;

    void push_to_free_list(void* ptr);
    void* allocate_block(uint16_t channel, uint64_t index0, uint64_t index1);

    bool is_mmap_slot_fresh(uint16_t channel, uint64_t global_block_seq) const;
    void mark_mmap_slot_written(uint16_t channel, uint64_t global_block_seq);
    void clear_mmap_slot_written(uint16_t channel, uint64_t global_block_seq);
    void clear_mmap_slot_by_abs(uint64_t abs_slot);

private:
    std::vector<std::vector<struct RootNode>> _ch_data;
    uint8_t     _byte_fraction;
    uint16_t    _ch_fraction;
    uint8_t    *_dest_ptr;

    uint64_t    _last_sample[CHANNEL_MAX_COUNT];
    uint64_t    _last_calc_count[CHANNEL_MAX_COUNT];
    bool        _is_loop;
    uint64_t    _loop_offset;
    bool        _able_free;
    std::vector<void*> _free_block_list;
    struct BlockIndex _cur_ref_block_indexs[CHANNEL_MAX_COUNT];
    int         _lst_free_block_index;

    // mmap-backed chunk allocator state (cluster A — heavily used by
    // allocate_block / copy_from / push_to_free_list / free_data / first_payload).
    // _disk_cache_config + _mmap_slot_written moved to LogicSnapshotDiskCacheWriter.
    std::shared_ptr<MmapAllocator> _mmap_alloc;
    uint64_t _max_blocks_per_channel;

    std::atomic<uint64_t> _last_pf_count{0};
    std::atomic<int64_t> _last_pf_time{0};
    std::atomic<uint64_t> _pf_per_sec{0};

    // Extracted disk-cache/async-writer subsystem (cluster D). Declared LAST so
    // its destructor (which joins the async writer thread) runs FIRST — before
    // _mmap_alloc / _ch_data are destroyed. mutable so const forwarders can
    // call non-const writer methods.
    mutable std::unique_ptr<LogicSnapshotDiskCacheWriter> _disk_cache_writer;

    // Extracted glitch-filter subsystem (cluster C). mutable so const
    // forwarders (e.g. is_glitch_filtered / get_filtered_ranges) can call
    // non-const methods on the helper.
    mutable std::unique_ptr<LogicSnapshotGlitchFilter> _glitch_filter;

    // P1-6 fix: Iterator reference count — prevents free_data/free_head_blocks
    // from freeing memory while get_samples() or other readers are active.
    // Matches PulseView's Segment::iterator_count_ mechanism.
    std::atomic<int> _iterator_count{0};

    friend class ::LogicSnapshotDiskCacheWriter;
    friend class LogicSnapshotGlitchFilter;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_LOGICSNAPSHOT_H
