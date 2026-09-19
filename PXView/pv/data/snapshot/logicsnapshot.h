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
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <bit>      // std::countr_zero / std::bit_width (see lsb_index / msb_index)
#include <cstdint>
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

// Extracted pattern-search subsystem. Defined in
// logicsnapshot_pattern_search.h/.cpp. Forward-declared here; LogicSnapshot
// holds it via unique_ptr and forwards the public pattern_search method.
class LogicSnapshotPatternSearch;

// Extracted display-edge scan subsystem. Defined in
// logicsnapshot_edge_scan.h/.cpp. Forward-declared here; LogicSnapshot
// holds it via unique_ptr and forwards the three public edge-scan methods
// (get_display_edges / get_nxt_edge / get_pre_edge).
class LogicSnapshotEdgeScan;

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

    // ------------------------------------------------------------------
    // Edit visibility: reader / writer guards for the in-place edit pass
    // ------------------------------------------------------------------
    // The glitch filter and signal invert rewrite leaf-block content IN PLACE
    // (bytes + mipmap + tog/first/last). On a FINITE capture the public
    // readers bypass _mutex entirely (they synchronize on
    // committed_sample_count()), so without these guards a renderer could
    // observe a block whose `tog` bit had been cleared but whose bytes were
    // only half written, and paint a garbage waveform for a frame.
    //
    // Semantics: readers must never wait for more than one edit BATCH; the
    // writer is a background thread, so making it wait for readers costs
    // nothing user-visible.

    /// Owner of the shared (reader) side. While no edit transaction is open
    /// this acquires nothing at all; it only checks the revision counter.
    class EditReadGuard {
    public:
        struct ForceLock {};

        explicit EditReadGuard(const LogicSnapshot *s) noexcept : _snap(s) {
            _epoch = _snap->_edit_epoch.load(std::memory_order_acquire);
            if (_epoch & 1u) {
                // An edit transaction is open. Taking the shared lock makes us
                // wait for it (and excludes the next one), after which the
                // content is a complete revision again.
                _snap->_edit_visibility.lock_shared();
                _locked = true;
            }
        }

        EditReadGuard(const LogicSnapshot *s, ForceLock) noexcept : _snap(s) {
            _snap->_edit_visibility.lock_shared();
            _locked = true;
            _epoch = _snap->_edit_epoch.load(std::memory_order_acquire);
        }

        ~EditReadGuard() {
            if (_locked)
                _snap->_edit_visibility.unlock_shared();
        }

        EditReadGuard(const EditReadGuard &) = delete;
        EditReadGuard &operator=(const EditReadGuard &) = delete;

        /// True when the read performed under this guard is guaranteed to have
        /// observed one complete revision (either because we held the lock, or
        /// because no writer opened a transaction while we read).
        bool consistent() const noexcept {
            return _locked ||
                   _snap->_edit_epoch.load(std::memory_order_acquire) == _epoch;
        }

    private:
        const LogicSnapshot *_snap;
        uint64_t _epoch = 0;
        bool _locked = false;
    };

    /// Multi-call read pin.
    ///
    /// consistent_read() gives per-CALL consistency: one get_sample() or
    /// get_display_edges() never observes a torn revision. But a caller that
    /// makes many of those calls in a loop — a pulse scan, a CSV export, a
    /// pattern search — can still straddle an edit revision and compute a
    /// result stitched from two revisions. No individual read is wrong; the
    /// aggregate is. Those are exactly the callers whose output is a durable
    /// artifact (exported text, a measured histogram), so a transient mix is
    /// not acceptable.
    ///
    /// Holding a pin for the whole scan closes that hole: it takes the shared
    /// visibility lock, so an edit batch waits for the scan to finish instead
    /// of interleaving into the middle of it. Making the WRITER wait is free —
    /// it is a background thread; making the renderer wait would not be.
    ///
    /// Cost is one shared-lock acquisition for the whole scan, not per call,
    /// and the inner consistent_read() paths do not nest (while we hold the
    /// pin no writer can be inside, so the edit epoch reads even and
    /// EditReadGuard skips the lock — which is also what keeps this from
    /// self-deadlocking on a non-recursive std::shared_mutex).
    ///
    /// Use it for long scans, NOT for per-frame render paths: pinning during a
    /// paint would stall the writer once per frame.
    class EditReadPin {
    public:
        explicit EditReadPin(const LogicSnapshot *s) noexcept : _snap(s) {
            _snap->_edit_visibility.lock_shared();
        }
        ~EditReadPin() { _snap->_edit_visibility.unlock_shared(); }
        EditReadPin(const EditReadPin &) = delete;
        EditReadPin &operator=(const EditReadPin &) = delete;

    private:
        const LogicSnapshot *_snap;
    };

    /// Owner of the exclusive (writer) side. Held for one batch / one revert
    /// chunk. Lock order: _mutex first, then this.
    class EditWriteGuard {
    public:
        explicit EditWriteGuard(LogicSnapshot *s) noexcept : _snap(s) { open(); }

        ~EditWriteGuard() { close(); }

        EditWriteGuard(const EditWriteGuard &) = delete;
        EditWriteGuard &operator=(const EditWriteGuard &) = delete;

        /// Close and immediately reopen this transaction, so that a reader can
        /// interleave between two chunks of one long in-place operation.
        ///
        /// WHY THIS EXISTS: the contract above (logicsnapshot.h, "readers must
        /// never wait for more than one edit BATCH") is what keeps the GUI alive
        /// while a background edit runs. A reader that arrives while a
        /// transaction is open blocks in lock_shared() until the writer closes
        /// it, so an operation that holds ONE transaction for its whole duration
        /// (the undo, the signal invert) makes every render wait for the whole
        /// operation. Publishing per chunk bounds that wait at one chunk.
        ///
        /// PRECONDITIONS:
        ///  - The state published by this call must be SELF-CONSISTENT: for every
        ///    block the current chunk touched, bytes + mipmap + tog/first/last
        ///    must already be in their final form for this chunk. A reader gets in
        ///    the instant the lock is dropped.
        ///  - This thread must be the only writer at this moment (guaranteed for
        ///    the filter/invert/clear paths by FilterProcessor::_edit_mutex).
        ///    Otherwise two writers could interleave chunk by chunk.
        ///  - Call it only from the thread that owns this guard.
        ///
        /// A reader already waiting on lock_shared() is typically admitted at
        /// the next close(), i.e. after at most one chunk — but treat this as
        /// best-effort, not a guarantee: std::shared_mutex has no fairness
        /// contract, so a writer that immediately re-acquires may in theory
        /// barge across several chunks and starve queued readers.
        /// Empirically one chunk; a hard bound would need a writer-side
        /// handshake that is not worth the complexity (see the
        /// chunk-granularity note in logicsnapshot_glitch_filter.cpp).
        void publish() noexcept {
            close();
            open();
        }

    private:
        void open() noexcept {
            _snap->_edit_visibility.lock();
            // Declare the transaction open. Both increments happen under the
            // exclusive lock, so readers holding the shared lock never observe
            // the counter move.
            _snap->_edit_epoch.fetch_add(1, std::memory_order_release);
            _snap->_edit_write_depth.fetch_add(1, std::memory_order_relaxed);
        }

        void close() noexcept {
            _snap->_edit_write_depth.fetch_sub(1, std::memory_order_relaxed);
            _snap->_edit_epoch.fetch_add(1, std::memory_order_release);
            // Still exclusive here, and the transaction's own revision is
            // already published — so this is the point where the storage the
            // transaction detached can actually go back to the allocator
            // (see push_to_free_list(): inside a transaction releases are only
            // DEFERRED, because a block that is being freed must never be
            // decommitted or recycled while a reader may still hold a pointer
            // into it).
            _snap->flush_deferred_free_list();
            _snap->_edit_visibility.unlock();
        }

        LogicSnapshot *_snap;
    };

    /// Undo an overlapping edit revision: run the read, and if a writer
    /// slipped in mid-read, redo it. After a bounded number of losses the
    /// shared lock is taken so the retry cannot spin indefinitely.
    template <typename ReadFn>
    auto consistent_read(ReadFn &&fn) -> decltype(fn()) {
        for (int attempt = 0; attempt < kEditReadRetries; ++attempt) {
            EditReadGuard guard(this);
            decltype(fn()) result = fn();
            if (guard.consistent())
                return result;
        }
        EditReadGuard guard(this, EditReadGuard::ForceLock{});
        return fn();
    }


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
    // P1-c: 统一读取抽象（见 pv/data/snapshot/sample_span.h）。
    SampleSpan span(uint32_t channel, uint64_t start, uint64_t count) const override;


    bool get_sample(uint64_t index, int sig_index);

    void capture_ended();

    bool get_display_edges(std::vector<std::pair<bool, bool>> &edges,
                           std::vector<std::pair<uint16_t, bool>> &togs,
                           uint64_t start, uint64_t end, uint16_t width,
                           uint16_t max_togs, double pixels_offset,
                           double min_length, uint16_t sig_index);

    bool get_nxt_edge(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);

    bool get_pre_edge(uint64_t &index, bool last_sample,
                      double min_length, int sig_index);

    // Bit-invert one channel in place (rebuilds the mipmap of every block).
    //
    // `progress_callback` (optional) fires once per published chunk, i.e. every
    // N leaf blocks, right after the exclusive transaction was closed and
    // reopened (EditWriteGuard::publish) — so a caller observes a revision that
    // is already complete and a reader is free to read it.
    //
    // Same contract as the other edit callbacks: it runs inside the edit
    // transaction and must neither block nor read this snapshot (taking an
    // EditReadGuard on this thread would self-deadlock — std::shared_mutex is not
    // recursive).
    void invert_channel(int sig_index,
                        std::function<void()> progress_callback = nullptr);
    // `cancel` (optional, polled once per scan iteration) lets a config/capture
    // boundary make an in-flight pass finish promptly instead of blocking for
    // the whole pass. Results are then partial — the caller must revert.
    void apply_glitch_filter(int sig_index, uint32_t threshold, std::function<void(int)> progress_callback,
        GlitchFilterMode filter_mode = GlitchFilterMode::Both,
        const std::atomic<bool> *cancel = nullptr,
        std::function<void()> batch_callback = nullptr);
    // `batch_callback` (optional) fires once per committed write batch so
    // callers can publish "the sample store has a new visible revision" to the
    // UI. It must neither block nor read this snapshot — it runs inside the
    // batch's exclusive EditWriteGuard. See
    // LogicSnapshotGlitchFilter::apply_glitch_filter.
    //
    // EDIT-PASS SCOPE: this is NOT a scope opener. The edit-pass failure scope
    // is opened by revert_all_edits() alone — the single point where a pass
    // starts from capture-original data. Calling this on top of an existing
    // pass (without reverting) deliberately keeps that pass's failure state:
    // that is what makes "revert first, then re-filter" one atomic scope, and
    // what lets a Core pass abort on a failed revert instead of masking it
    // (see FilterProcessor::rebuild_filtered_state). Data-layer callers that
    // want a clean scope must revert first — which they must do anyway to
    // avoid stacking edits.
    void apply_glitch_filter_all(const std::map<int, uint32_t> &thresholds, std::function<void(int)> progress_callback,
        const std::map<int, GlitchFilterMode> &filter_modes = {},
        const std::atomic<bool> *cancel = nullptr,
        std::function<void()> batch_callback = nullptr);
    bool is_glitch_filtered();
    // NOTE: the former set_glitch_filtered(bool) has been removed — it had no
    // callers (the state is maintained internally by apply_glitch_filter_all /
    // revert_all_edits and only ever READ from outside, by the renderer).

    // 持久化访问 apply_glitch_filter 滤除的区间列表，供 View 层渲染 overlay。
    // 返回不可变表的 shared_ptr：读者持有期间表不会被改写或替换（旧 API
    // 返回 const& 指向 worker 正在 push_back 的 vector，扩容即悬垂）。
    // 表按 start 升序，调用方应对可见窗口做二分而不是每帧全量遍历。
    std::shared_ptr<const std::vector<FillRange>> get_filtered_ranges(
        int sig_index) const;
    void clear_filtered_ranges();

    // 把快照恢复到"采集原始数据"：撤销所有毛刺滤波 / 信号反相编辑。
    // 取代原先"整份快照深拷贝到 _logic_backup，再 copy_from 回来"的撤销
    // 方案（8 通道 × 1 G 采样 = 976 MB + 每次重滤一整趟 480 次 commit）。
    // 代价正比于真正被改写的字节数（每个被抹平的毛刺只有几个采样）。
    // 幂等：没有编辑时是空操作。
    // 返回 false 表示至少一条记录未能还原（重实体化时内存池分配失败）——
    // 调用方必须视为"快照未回到采集原始态"，不得上报成功，也不得在其上
    // 继续叠加新编辑；同时入口会**开启新的编辑趟作用域**（复位
    // edit_pass_failed，见 logicsnapshot_glitch_filter.h 的 SCOPE NOTE）。
    // 这是全库唯一的趟作用域开启点：编辑路径以它开头，采集层的
    // memory_failed 与它无关。
    // `progress_callback` (optional; must not block or read this snapshot, it
    // runs inside the revert's exclusive edit transaction) fires periodically
    // during a long undo. When that notice becomes visible depends on the
    // transaction being published between chunks — see the visibility note in
    // LogicSnapshotGlitchFilter::revert_all_edits.
    bool revert_all_edits(std::function<void()> progress_callback = nullptr);
    /// Edit-log observability query. NOT used by the Core edit paths any more
    /// (they decide with edit_pass_aborted()); kept because it is the only way
    /// to ask "is this snapshot currently carrying un-reverted edits", which
    /// is what the save/load and regression tests assert on.
    bool has_filter_edits() const;
    /// True when the reversible edit log hit its memory budget during the
    /// last pass, i.e. the filter bailed out mid-way. Callers must revert
    /// and report a failure instead of accepting a partially filtered
    /// snapshot. Cleared by revert_all_edits().
    ///
    /// Kept SEPARATE from edit_pass_failed() because it has a second, lasting
    /// effect beyond the failure report: while it is set, recording is
    /// disabled, so apply_glitch_filter() refuses to run (it would modify data
    /// it could no longer undo) until a revert clears it.
    bool edit_log_overflowed() const;
    /// True when the CURRENT edit pass (revert / invert / glitch filter) hit
    /// an allocation failure. Deliberately SEPARATE from the inherited
    /// memory_failed(): that flag is the CAPTURE-pipeline degradation signal
    /// (DataFeedParser drops packets and stops the capture on it), while this
    /// one only reports "this edit pass could not allocate".
    ///
    /// The pass scope is opened by revert_all_edits() ALONE (the single point
    /// where a pass returns to capture-original data) and is not touched by
    /// the capture path or by apply_glitch_filter_all() — see the scope note
    /// on that method. Consequently a transient OOM in one pass never poisons
    /// later passes, and no edit operation can disarm capture semantics.
    bool edit_pass_failed() const;
    /// Single predicate for "this edit pass did not produce a usable result":
    /// an allocation failure OR an exceeded edit-log budget. This is what the
    /// Core checks at every pass exit, so the definition of "the pass failed"
    /// lives in one place instead of being re-spelled at each call site. The
    /// two underlying states stay separately queryable for diagnosis (and
    /// because the overflow state has the lasting effect described above).
    bool edit_pass_aborted() const;

    void set_disk_cache_config(const DiskCacheConfig &config);
    bool is_disk_cache_active();
    double get_disk_write_speed_mbps();
    size_t get_disk_write_queue_depth();
    uint64_t get_disk_total_blocks_written();
    uint64_t get_page_fault_count();
    uint64_t get_working_set_bytes();
    uint64_t get_async_queue_bytes();

    // raw 版内存/磁盘指示口径（raw 复原 spec，用户拍板）：
    // 逻辑落盘字节 = 已采集样本数/8（8 samples/byte），磁盘占用 = mmap 分配器文件大小。
    uint64_t get_mmap_total_bytes();

    // P1-b（零拷贝生命周期契约）：把 mmap 区域提升为可被读者"钉住"的引用计数句柄。
    //
    // 背景：解码线程通过 libsigrokdecode 持有裸内部指针（di->inbuf）指向 mmap 区域，
    // 但这些指针的生命周期此前只由 LogicSnapshot 的私有成员 _mmap_alloc 决定 ——
    // 一旦该成员被 reset / 重新赋值，映射被 munmap/UnmapViewOfFile，裸指针立即悬垂。
    // 已有的 _iterator_count 守卫只保护 leaf block 的释放，不保护映射本身。
    //
    // 现在任何读者（解码线程、渲染、测量）都可以取一份拷贝并在读取期间持有它，
    // 引用计数归零前映射不可能被解除。返回 nullptr 表示当前没有 mmap 后端
    // （配置失败回退到 LeafBlockPool，或尚未 first_payload）。
    std::shared_ptr<MmapAllocator> mmap_region() const { return _mmap_alloc; }

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
    /// Number of live get_samples()/SegmentDataIterator readers. For logging
    /// the edit pass's bounded drain.
    inline int active_iterator_count() const {
        return _iterator_count.load(std::memory_order_acquire);
    }

    /// Capture-boundary drain: block (bounded) until no active sample
    /// iterators remain. Returns true if drained within the window.
    /// A decode thread from the PREVIOUS capture may still hold a get_samples()
    /// iterator, which makes free_data() defer freeing and accumulate old
    /// capture buffers across a long session (group3 OOM / std::bad_alloc).
    /// Call before free_data() at a capture boundary so the free actually runs.
    bool wait_active_iterators_zero(int max_wait_ms = 3000);

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

    std::unique_ptr<SegmentDataIterator> begin_sample_iteration(uint64_t start, int sig_index);
    void continue_sample_iteration(SegmentDataIterator* it, uint64_t increase);
    void end_sample_iteration(std::unique_ptr<SegmentDataIterator> it);
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

    // C3 (P9-on-raw): committed sample count for a lock-free reader.
    // FINITE (non-loop) mode: release-published `_ring_published` — a reader
    // acquire-loads it and only ever observes fully-committed samples (mipmap
    // metadata + leaf data written before the release-store). LOOP/∞ mode:
    // returns `_ring_sample_count` — caller MUST hold `_mutex` (loop mode
    // rotates + frees blocks and rebases via `_loop_offset`, so reads need
    // the lock). `_loop_offset` is 0 for finite captures, so never read here.
    inline uint64_t committed_sample_count() const {
        return _is_loop ? _ring_sample_count
                        : _ring_published.load(std::memory_order_acquire);
    }

    static int get_block_with_sample(uint64_t index, uint64_t *out_offset);

private:
    bool get_sample_unlock(uint64_t index, int sig_index);
    bool get_sample_self(uint64_t index, int sig_index);

    // P5 diff 扫描 (spec 阶段3): 直接对 raw 块字节做 u64 差分 + ctz
    // (lsb_index / std::countr_zero) 定位 [start, end] 内第一个与 expected_level 不同的采样点,
    // 输出到 out_pos 并返回 true. 相比 mipmap 树搜索 (get_nxt_edge_self),
    // 稠密数据下 O(bytes) 常量级扫描, 供毛刺滤波主循环复用 (spec:
    // "毛刺滤波走 diff+ctz, 吞吐不低于 RLE 版"). order 是 _ch_data 通道序,
    // 调用方保证 start 处电平 == expected_level.
    bool find_first_different_raw(int order, uint64_t start, uint64_t end,
                                  bool expected_level, uint64_t &out_pos);

    int get_ch_order(int sig_index) const;

    void calc_mipmap(unsigned int order, uint8_t index0, uint8_t index1, uint64_t samples, bool isEnd);

    void append_payload_impl(const sr_datafeed_logic &logic);

    /** Append raw channel-block (LA_CROSS_DATA) payload. v1.49 algorithm:
     *  bit-copy raw bytes directly into per-channel chunk tree, no
     *  deinterleave. Called by DiskCacheWriter async worker when
     *  logic.format == LA_CROSS_DATA. */
    void append_cross_payload(const sr_datafeed_logic &logic);

    // ---- Bit scan helpers (C++20 <bit>) ---------------------------------
    //
    // These replace the hand-rolled De Bruijn bit scan that used to live here
    // (`bsf_folded` + a 64-entry table, `bsr32` + a 256-entry table). GCC/Clang
    // lower std::countr_zero / std::bit_width to the very same `bsf` / `lzcnt`
    // instruction the tables existed to avoid computing (verified equivalent
    // over every single-bit / mask value, all 256 byte inputs and 6 M random
    // sparse+dense values before the swap), so the hot mipmap walks and the raw
    // diff scan keep their throughput and lose ~50 lines of magic numbers.
    //
    // Both keep the ZERO-INPUT CONVENTION of the code they replace: `bsf_folded(0)`
    // returned 63 and `bsr64(0)` returned 0 (an artifact of the lookup tables),
    // and call sites compute these positions BEFORE testing the mask for zero.
    // Preserving those answers keeps that pattern valid; do not "fix" them to
    // 64 / to an underflowing bit_width(0)-1 without auditing every caller.

    /// Index of the least significant set bit (0..63); 63 for a zero input.
    static inline uint8_t lsb_index(uint64_t bb) noexcept
    {
        return static_cast<uint8_t>(bb ? std::countr_zero(bb) : 63);
    }

    /// Index of the most significant set bit (0..63); 0 for a zero input.
    static inline uint8_t msb_index(uint64_t bb) noexcept
    {
        return static_cast<uint8_t>(bb ? (std::bit_width(bb) - 1) : 0);
    }

    void move_first_node_to_last();

    void free_head_blocks(int count);

// B-6: Override to connect PulseView-style free_unused_memory() to
// the existing free_head_blocks() mechanism. Called by Snapshot::
// set_complete() (via _mem_optimization_requested) after capture ends.
void free_unused_memory() override;

    // Hand a detached leaf block back to the allocator.
    //
    // INSIDE AN EDIT TRANSACTION the release is DEFERRED (parked in
    // _edit_deferred_free and flushed when the transaction closes) instead of
    // being performed inline. The edit passes detach published blocks:
    //   - calc_mipmap() collapses a block that the filter flattened to a
    //     constant (RLE representation), and
    //   - revert_all_edits() drops a block that the pass had materialised.
    // Both run while readers may hold pointers obtained before the pass
    // started (the decoder's get_samples() raw pointers, and — in stream
    // sessions, where `_able_free` is true — the lock-free
    // committed_sample_count() readers). Releasing inline would decommit an
    // mmap block (VirtualFree(MEM_DECOMMIT) — a later access is an access
    // violation, not "the old content") or park a pool block where the next
    // allocate_block() hands it to somebody else (a later read returns
    // unrelated data). Deferring is what makes "a published block is never
    // freed or unmapped" true for the whole edit path instead of only for the
    // part guarded by _able_free.
    void push_to_free_list(void* ptr);
    /// The inline half of push_to_free_list(): mmap blocks are decommitted,
    /// pool blocks are parked in _free_block_list. Never call this directly
    /// from inside an edit transaction.
    void release_block_now(void* ptr);
    /// Flush _edit_deferred_free through release_block_now(). Safe only when
    /// no reader can be inside — i.e. while _edit_visibility is held
    /// exclusively (EditWriteGuard::close) or at teardown (free_data).
    void flush_deferred_free_list();
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
    // std::atomic：写方是数据馈送线程（DataFeedParser 在每场采集开始时经
    // set_loop() 置位，见 datafeedparser.cpp 的 last_ended() 分支），读方遍布
    // 采样/迭代/边沿扫描/渲染等 10+ 处**无锁**读——它们据此决定"走 _mutex 路径
    // 还是走 _ring_published 无锁发布路径"。普通 bool 下这一写多读没有任何同步，
    // 读者可能看到翻转前后的中间态而选错路径。与 _memory_failed/_last_ended
    // 同一约定（见 snapshot.h 的说明）。
    std::atomic<bool> _is_loop;
    uint64_t    _loop_offset;

    // C3 (P9-on-raw): committed-sample-count publication for lock-free FINITE
    // (non-loop) readers. The data-feed thread writes `_ring_sample_count`
    // and finishes calc_mipmap for a region BEFORE release-storing this
    // atomic; lock-free readers acquire-load it, so they only ever observe
    // fully-committed samples and never race with in-place mipmap metadata
    // mutation or with blocks still being written. Loop/∞ mode keeps the
    // existing `_mutex` path (it rotates + frees blocks), so this atomic is
    // only advanced for finite captures. `_loop_offset` is 0 for finite
    // captures, so lock-free readers never read it.
    std::atomic<uint64_t> _ring_published{0};
    bool        _able_free;
    std::vector<void*> _free_block_list;
    // Blocks detached WHILE an edit transaction was open. Released by
    // flush_deferred_free_list() when the transaction closes (or at teardown),
    // never inline — see push_to_free_list().
    std::vector<void*> _edit_deferred_free;
    struct BlockIndex _cur_ref_block_indexs[CHANNEL_MAX_COUNT];
    int         _lst_free_block_index;

    // mmap-backed chunk allocator state (cluster A — heavily used by
    // allocate_block / push_to_free_list / free_data / first_payload).
    // _disk_cache_config + _mmap_slot_written moved to LogicSnapshotDiskCacheWriter.
    std::shared_ptr<MmapAllocator> _mmap_alloc;
    uint64_t _max_blocks_per_channel;

    // P1-b: 记录 _mmap_alloc 被 configure 时的几何，供 first_payload 判断
    // "能否复用现有区域"。free_data() 在迭代器活跃时会提前返回（不 reset
    // _mmap_alloc），此时若通道数/深度已变，旧代码会带着旧几何进入复用分支，
    // 而 allocate_block() 按 _max_blocks_per_channel 计算 mmap 槽位 —— 会写错位置。
    // 0 表示当前 _mmap_alloc 为空或几何未知。
    uint64_t _mmap_geom_channel_num = 0;
    uint64_t _mmap_geom_max_blocks = 0;

    std::atomic<uint64_t> _last_pf_count{0};
    std::atomic<int64_t> _last_pf_time{0};
    std::atomic<uint64_t> _pf_per_sec{0};

    // ---- Edit-visibility lock ------------------------------------------
    // The glitch filter and signal invert rewrite leaf-block content IN PLACE
    // (bytes + mipmap + tog/first/last). On a FINITE capture the public
    // readers bypass _mutex entirely — they synchronize on
    // committed_sample_count() — so without this lock a renderer could
    // observe a block whose `tog` bit had been cleared but whose bytes were
    // only half written, and paint a garbage waveform for a frame.
    //
    // Readers take it SHARED, the edit writer takes it EXCLUSIVE for the
    // duration of ONE BATCH (not the whole pass). A reader therefore never
    // waits longer than a single batch, which is what keeps the GUI
    // responsive; the writer is a background thread, so making it wait for
    // readers costs nothing user-visible.
    //
    // Deliberately NOT _mutex: that one is recursive and guards the
    // capture/structure lifecycle (its recursion is genuinely relied upon by
    // recalc_mipmap -> calc_mipmap and revert_all_edits -> invert_channel),
    // and readers must be able to enter concurrently with each other.
    //
    // MUST be acquired after _mutex, never before (writer order is
    // _mutex -> _edit_visibility); readers take only this one, so there is no
    // cycle. Do not nest acquisitions of this lock in one thread:
    // std::shared_mutex is not recursive and a shared -> shared upgrade
    // behind a waiting writer would deadlock.
    //
    // NOT covered by this lock: get_samples() iterators. They hand out a raw
    // pointer that the decoder keeps reading after the call returns, so no
    // per-call lock can protect it — that is the pre-existing `_able_free`
    // contract. Edit passes additionally drain iterators (bounded) before
    // writing; see LogicSnapshotGlitchFilter::apply_glitch_filter.
    mutable std::shared_mutex _edit_visibility;

    // Edit revision counter. ODD = an edit transaction is open (readers must
    // synchronize); EVEN = the content is a complete, self-consistent
    // revision. It is only ever modified while _edit_visibility is held
    // exclusively, which is what makes the unlocked reader fast path below
    // sound: a reader that holds nothing can detect "a writer slipped in" by
    // re-reading this, and a reader that holds the shared lock cannot see it
    // change at all.
    //
    // This exists because taking the shared lock unconditionally on every
    // sample read is far too expensive: it measured 3-7x slower on the data
    // query paths (millions of get_sample()/pattern_search() calls). With the
    // counter, the common case costs two acquire loads instead of a lock.
    std::atomic<uint64_t> _edit_epoch{0};
    static constexpr int kEditReadRetries = 8;

    // Debug-build conformance counter: non-zero exactly while an
    // EditWriteGuard is alive. Every in-place writer of published leaf data
    // asserts on it (see edit_write_owned()), so a future writer that forgets
    // the guard fails loudly in debug instead of silently reintroducing the
    // torn-revision read this whole mechanism exists to prevent.
    std::atomic<int> _edit_write_depth{0};

    // Edit-pass-local allocation-failure flag (see edit_pass_failed()). Set by
    // the glitch-filter subsystem (a friend) when a block re-materialisation
    // or materialisation fails mid-pass. Reset ONLY by revert_all_edits(), the
    // single edit-pass scope opener (apply_glitch_filter_all deliberately does
    // not reset it — see that method's scope note). NEVER set or cleared by the
    // capture path — that is what the inherited Snapshot::_memory_failed is for.
    std::atomic<bool> _edit_pass_failed{false};

public:
    /// True while this thread (or any thread) holds an EditWriteGuard on this
    /// snapshot. Used by the in-place write sites as a debug conformance check;
    /// do not branch on it in production logic.
    bool edit_write_owned() const {
        return _edit_write_depth.load(std::memory_order_relaxed) > 0;
    }

    /// True while an edit transaction is open on this snapshot, i.e. while its
    /// leaf blocks are being rewritten in place by the glitch filter, the
    /// signal invert, or the undo pass.
    ///
    /// This IS meant to be branched on from the render path. A guarded read
    /// taken while a transaction is open BLOCKS until the writer finishes
    /// (EditReadGuard takes the shared lock in that case), and the undo holds a
    /// single transaction for its whole duration — seconds on a large capture.
    /// A paint that reached get_display_edges() during that window froze the
    /// GUI thread inside paintEvent, which the window manager reports as "not
    /// responding", even though the work itself now runs on a worker thread.
    ///
    /// The renderer therefore checks this first and skips its signal-pixmap
    /// rebuild for the frame (the cached pixmap is a complete, self-consistent
    /// revision; the dirty flag stays set), and paints the new revision on the
    /// first frame after the writer publishes or finishes. One relaxed atomic
    /// load — cheaper than any lock, and never blocking.
    bool edit_in_progress() const noexcept {
        return (_edit_epoch.load(std::memory_order_acquire) & 1u) != 0;
    }

    /// Number of transaction open/close transitions on this snapshot (two per
    /// transaction: one when it opens, one when it closes). Exposed so tests can
    /// assert that a long operation really PUBLISHED between chunks — i.e. that
    /// the exclusive side was released and re-taken instead of being held for the
    /// whole operation. Monotonic; only the writer moves it.
    uint64_t edit_revision() const noexcept {
        return _edit_epoch.load(std::memory_order_acquire);
    }

private:

    // Extracted disk-cache/async-writer subsystem (cluster D). Declared LAST so
    // its destructor (which joins the async writer thread) runs FIRST — before
    // _mmap_alloc / _ch_data are destroyed. mutable so const forwarders can
    // call non-const writer methods.
    mutable std::unique_ptr<LogicSnapshotDiskCacheWriter> _disk_cache_writer;

    // Extracted glitch-filter subsystem (cluster C). mutable so const
    // forwarders (e.g. is_glitch_filtered / get_filtered_ranges) can call
    // non-const methods on the helper.
    mutable std::unique_ptr<LogicSnapshotGlitchFilter> _glitch_filter;

    // Extracted pattern-search subsystem. mutable so const forwarders can call
    // non-const methods on the helper.
    mutable std::unique_ptr<LogicSnapshotPatternSearch> _pattern_search;

    // Extracted display-edge scan subsystem. mutable so const forwarders can
    // call non-const methods on the helper.
    mutable std::unique_ptr<LogicSnapshotEdgeScan> _edge_scan;

    // P1-6 fix: Iterator reference count — prevents free_data/free_head_blocks
    // from freeing memory while get_samples() or other readers are active.
    // Matches PulseView's Segment::iterator_count_ mechanism.
    std::atomic<int> _iterator_count{0};

    friend class ::LogicSnapshotDiskCacheWriter;
    friend class LogicSnapshotGlitchFilter;
    friend class LogicSnapshotPatternSearch;
    friend class LogicSnapshotEdgeScan;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_LOGICSNAPSHOT_H
