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

#ifndef PXVIEW_PV_DATA_LOGICSNAPSHOT_GLITCH_FILTER_H
#define PXVIEW_PV_DATA_LOGICSNAPSHOT_GLITCH_FILTER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// This header needs the full definition of LogicSnapshot because the public
// API references the nested type LogicSnapshot::FillRange (kept on
// LogicSnapshot so external callers continue to use
// `LogicSnapshot::FillRange`). The dependency is one-way:
// logicsnapshot.h forward-declares LogicSnapshotGlitchFilter and holds it via
// unique_ptr; this header includes logicsnapshot.h to obtain FillRange.
#include "pv/data/snapshot/logicsnapshot.h"

namespace pv {
namespace data {

// Extracted glitch-filter subsystem (cluster C) from LogicSnapshot.
// Owns the per-channel filtered-range list, the glitch-filter state flag, and
// the apply/invert/recalc logic. LogicSnapshot holds this via unique_ptr and
// forwards the public methods; chunk-tree state (cluster A) stays in
// LogicSnapshot and is accessed through the back-pointer (friend).
class LogicSnapshotGlitchFilter
{
public:
    explicit LogicSnapshotGlitchFilter(LogicSnapshot *host);
    ~LogicSnapshotGlitchFilter();

    // ---- Glitch filter API (forwarded by LogicSnapshot) ----
    //
    // `cancel`, when non-null, is polled once per scan iteration. A capture or
    // configuration boundary that has to invalidate the snapshot can raise it
    // to make an in-flight pass finish promptly instead of blocking the
    // boundary for the whole pass. The pass stops at the next iteration and
    // leaves a partially applied result — the caller is responsible for
    // reverting (revert_all_edits()).
    //
    // `batch_callback`, when non-null, is invoked once per COMMITTED write
    // batch, i.e. every time a group of in-place edits becomes one complete
    // visible revision. It exists so the layer above can tell the UI that the
    // sample store changed.
    //
    // Without it nothing on the GUI side marked the signal pixmap dirty while a
    // pass ran: capture has stopped (no data packets arrive), decode is paused,
    // and DataUpdated is only emitted once the whole pass ends. The renderer
    // therefore kept blitting its cached pixmap and the waveform stayed frozen
    // on the pre-filter picture until the user scrolled (which changes
    // scale/offset and forces a rebuild) — reported as "only refreshes when I
    // scroll".
    //
    // MUST NOT BLOCK, AND MUST NOT READ THIS SNAPSHOT: it is called while the
    // exclusive write transaction (EditWriteGuard) for the batch is still open.
    // Reading samples from inside it — get_sample / get_display_edges / any
    // consistent_read() — would take an EditReadGuard on the same thread and
    // self-deadlock, because std::shared_mutex is not recursive. The intended
    // implementation only posts an async notice to the GUI thread, throttled by
    // the caller.
    void apply_glitch_filter(int sig_index, uint32_t threshold,
                             std::function<void(int)> progress_callback,
                             GlitchFilterMode filter_mode = GlitchFilterMode::Both,
                             const std::atomic<bool> *cancel = nullptr,
                             std::function<void()> batch_callback = nullptr);
    // 架构修复：thresholds/modes 用 channel_index 作 key，消除 View/Core 位置序号错位
    void apply_glitch_filter_all(const std::map<int, uint32_t> &thresholds,
                                 std::function<void(int)> progress_callback,
                                 const std::map<int, GlitchFilterMode> &filter_modes = {},
                                 const std::atomic<bool> *cancel = nullptr,
                                 std::function<void()> batch_callback = nullptr);
    bool is_glitch_filtered() const;
    void set_glitch_filtered(bool filtered);

    // Persisted filtered ranges for View-layer overlay rendering.
    //
    // Returned by value as a shared_ptr to an IMMUTABLE table (see
    // _published_ranges). The previous API returned `const&` into a vector
    // that the worker thread was concurrently push_back()-ing into, so a
    // reallocation could hand the render thread a dangling iterator. With
    // publication the reader keeps a ref-counted snapshot alive for as long
    // as it needs it and can never observe a mutation.
    //
    // The returned table is sorted by `start` (the filter scans samples in
    // ascending order), so callers can binary-search the visible window
    // instead of walking every range per frame.
    std::shared_ptr<const std::vector<LogicSnapshot::FillRange>>
    get_filtered_ranges(int sig_index) const;
    void clear_filtered_ranges();

    // Signal invert (rebuilds mipmap per block).
    //
    // `on_chunk`, when non-null, is called every N leaf blocks. It is supplied by
    // whoever OWNS the edit transaction (LogicSnapshot::invert_channel for the
    // external entry, revert_all_edits for the un-invert step) and its job is to
    // publish the chunk (EditWriteGuard::publish) and optionally report progress
    // — this function never touches the lock itself, because its callers may
    // already be inside a transaction and std::shared_mutex is not recursive.
    void invert_channel(int sig_index, const std::function<void()> &on_chunk = {});

    // ------------------------------------------------------------------
    // Reversible edit log
    // ------------------------------------------------------------------
    // Every destructive write to leaf-block sample bytes (glitch-filter
    // fill, signal invert) is preceded by recording the bytes it is about
    // to overwrite. revert_all_edits() replays the log in reverse and
    // restores the capture-original content.
    //
    // This replaces the former "deep-copy the whole snapshot into
    // _logic_backup" undo strategy, which cost a second full copy of the
    // sample store (976 MB for 8 channels x 1 GS/s) plus a complete
    // MmapAllocator reset/unmap on every re-filter. Here the cost is
    // proportional to the number of bytes actually rewritten (a handful of
    // samples per removed glitch), and — crucially — no published block is
    // ever freed or unmapped, so the lock-free finite-capture readers keep
    // their "no block is freed during capture" invariant intact.
    // `progress_callback` (optional) is invoked once per published chunk, i.e.
    // every N leaf blocks, right after the exclusive transaction was closed and
    // reopened. Same contract as apply_glitch_filter's batch_callback: it must
    // neither block nor read this snapshot, because it runs inside that
    // transaction (taking an EditReadGuard here would self-deadlock).
    //
    // VISIBILITY: because each chunk is published, a reader may enter between
    // chunks (typically after waiting one chunk — see the fairness note on
    // EditWriteGuard::publish; std::shared_mutex gives no hard fairness
    // guarantee, so a barging writer could in theory keep a queued reader
    // waiting across several chunks) and the render path's "transaction in
    // progress" check (LogicSnapshot::edit_in_progress, see render_pass.cpp)
    // lets the frame through, so the undo is observable while it runs instead
    // of appearing as a frozen window. This is the same mechanism apply_batch()
    // uses per batch.
    //
    // Returns false when at least one record could not be restored (the pool
    // refused to re-materialise a collapsed block — OOM). The caller must
    // treat false as "the snapshot is NOT back at capture-original": do not
    // report success, do not build new edits on top, and let the user retry.
    //
    // SCOPE NOTE: the entry also clears the snapshot's edit-pass-local
    // edit_pass_failed() flag. Every FilterProcessor edit pass begins with
    // this call, so the reset marks the start of a pass: failures recorded
    // DURING the pass (revert / invert / filter) survive to the caller's
    // end-of-pass check, while a transient OOM from a PREVIOUS pass no longer
    // poisons every later pass (previously one OOM made filter/undo
    // permanently roll back until the next capture). This flag is
    // deliberately SEPARATE from the inherited memory_failed(), which stays
    // the CAPTURE-pipeline degradation signal (DataFeedParser drops packets
    // on it) and is never touched by edit passes.
    bool revert_all_edits(std::function<void()> progress_callback = nullptr);
    bool has_edits() const;
    /// Forget the edit log without restoring. Only for snapshot teardown
    /// (free_data / init_all), where the blocks are being dropped anyway.
    void clear_edits();
    /// True when the edit log hit its budget during the last pass, meaning
    /// the filter was aborted mid-way. Callers must revert and report a
    /// failure rather than leaving a partially filtered snapshot behind.
    bool edit_log_overflowed() const { return _edit_log_overflow; }

private:
    // Recompute mipmap for a single leaf block. Extracted from
    // LogicSnapshot (was private there) — used by invert_channel and
    // apply_glitch_filter's batch flush.
    void recalc_mipmap(unsigned int order, uint64_t index0, uint64_t index1);

    // Record the original bytes of [byte_lo, byte_hi) inside block
    // (order, idx0, idx1) of _host->_ch_data BEFORE they are overwritten.
    // `allocated` marks the RLE case where the block did not exist and was
    // just materialised — reverting then means freeing it again and
    // restoring the nullptr (unwritten/constant) representation.
    void record_edit(unsigned int order, uint64_t idx0, uint64_t idx1,
                     uint64_t byte_lo, uint64_t byte_hi, bool allocated);

    /// Estimated-cost gate for the edit log: payload + per-record overhead.
    /// Deliberately not a record COUNT cap — see kEditRecordOverheadBytes for
    /// the field bug a count cap caused (8.06M records / 11.6 MB of payload /
    /// ~900 MB of overhead / a large file that silently appeared unfiltered).
    bool edit_log_can_grow(uint64_t extra_payload, uint64_t extra_records) const;

    /// Total budget for the log, derived from the snapshot size so the guard is
    /// never stricter than the full-snapshot backup strategy this log replaced.
    uint64_t edit_log_budget_bytes() const;

    /// One reversible write. Replaying all records in reverse order
    /// restores the exact pre-edit byte stream, including the case where a
    /// single block was rewritten across multiple batches.
    struct EditRecord {
        uint32_t order = 0;
        uint64_t idx0 = 0;
        uint64_t idx1 = 0;
        uint64_t byte_lo = 0;          // offset inside the leaf data region
        bool     allocated = false;    // block was materialised by this edit
        std::vector<uint8_t> bytes;    // original bytes (empty when allocated)
        // Only meaningful for `allocated` records: recalc_mipmap() bails out
        // for a nullptr block, so the constant-value metadata (tog/first/last)
        // of the pre-materialisation RLE representation has to be restored
        // explicitly instead of being recomputed from the data.
        uint64_t meta_tog = 0;
        uint64_t meta_first = 0;
        uint64_t meta_last = 0;
    };

    LogicSnapshot *_host;

    bool        _glitch_filtered;

    // Reversible edit log. Appended by the worker while it writes; replayed
    // in reverse by revert_all_edits().
    std::vector<EditRecord> _edits;
    uint64_t _edit_bytes = 0;
    bool     _edit_log_overflow = false;
    // Leaf-block orders (indices into _host->_ch_data) that are currently
    // bit-inverted. Invert is an involution, so undoing it is running it
    // again — but only if we know it is still applied.
    std::vector<unsigned int> _inverted_orders;

    // Published filtered-range tables, one per signal index. Written only
    // after a channel's filter pass completes, so readers always observe a
    // complete table. _ranges_mutex is held for the duration of a pointer
    // copy by readers and a pointer swap by the writer, never across
    // filtering work.
    mutable std::mutex _ranges_mutex;
    std::map<int, std::shared_ptr<const std::vector<LogicSnapshot::FillRange>>> _published_ranges;
};

// ----------------------------------------------------------------------------
// 纯单通道毛刺滤波（决策逻辑）。从 apply_glitch_filter 抽出的可独立单元测试
// 的纯函数：给定一段单通道 0/1 电平样本与最小脉宽阈值，把"窄于（<=）阈值的
// 短脉冲毛刺"抹平为周围稳定电平，保留正常宽度沿。既不访问 LogicSnapshot 内部
// 状态，也不持有锁，因此无需实例即可测试。
//
// 说明：每个样本占 1 字节，值非 0 视为逻辑高、0 视为逻辑低；in 与 out 指向的
// 缓冲不得重叠（out 初始化为 in 的副本，再覆盖被判定为毛刺的区间）。
// filter_mode 语义与 apply_glitch_filter 相同：Both 滤除所有窄脉冲；
// High 仅在基准电平为高时滤除其上的窄低凹；Low 仅在基准电平为低时滤除窄高刺。
// ----------------------------------------------------------------------------
void apply_glitch_filter_one_pass(const uint8_t *in, uint8_t *out,
                                  uint64_t sample_count, uint32_t threshold,
                                  GlitchFilterMode filter_mode);

}  // namespace data
}  // namespace pv

#endif  // PXVIEW_PV_DATA_LOGICSNAPSHOT_GLITCH_FILTER_H
