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

// One-way dependency: this TU includes logicsnapshot.h (transitively via
// logicsnapshot_glitch_filter.h) and logicsnapshot_glitch_filter.h.
// logicsnapshot.h forward-declares LogicSnapshotGlitchFilter and holds it via
// unique_ptr<forward-declared type>; the forwarders live in logicsnapshot.cpp.

#include <algorithm>
#include <cassert>
#include <cstring>
#include <tuple>

#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/data/snapshot/leaf_block_pool.h"
#include "pv/data/snapshot/logicsnapshot_glitch_filter.h"

using namespace std;

namespace pv {
namespace data {

namespace {

// Safety valve for the reversible edit log. The log is what replaced the
// 976 MB full-snapshot backup, so it must itself be bounded: a pathologically
// dense glitch train could otherwise rewrite (and therefore have to save)
// every sample byte of the capture. When a budget is exceeded the filter
// aborts and reverts instead of silently degrading into a half-applied state.
constexpr uint64_t kMaxEditBytes = 256ull * 1024ull * 1024ull; // 256 MB
constexpr uint64_t kMaxEditRecords = 8ull * 1000ull * 1000ull; // 8 M

} // anonymous namespace

// ----------------------------------------------------------------------------
// Construction / destruction
// ----------------------------------------------------------------------------

LogicSnapshotGlitchFilter::LogicSnapshotGlitchFilter(LogicSnapshot *host)
    : _host(host)
    , _glitch_filtered(false)
{
}

LogicSnapshotGlitchFilter::~LogicSnapshotGlitchFilter()
{
}

// ----------------------------------------------------------------------------
// invert_channel / recalc_mipmap
// ----------------------------------------------------------------------------

// CALLER CONTRACT: the exclusive _edit_visibility lock must be held.
// This rewrites whole leaf blocks in place, so readers must be excluded — but
// it cannot take the lock itself, because revert_all_edits() already holds it
// for the whole revert transaction and std::shared_mutex is not recursive.
// External callers go through LogicSnapshot::invert_channel(), which takes it.
void LogicSnapshotGlitchFilter::invert_channel(int sig_index) {
  // Debug conformance check: XORs whole leaf blocks in place (see record_edit
  // for the rationale).
  assert(_host->edit_write_owned() &&
         "invert_channel() outside an EditWriteGuard transaction");

  std::lock_guard<std::recursive_mutex> lock(_host->_mutex);

  int order = _host->get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _host->_ch_data.size())
    return;

  if (_host->_ring_sample_count == 0)
    return;



  for (uint64_t i = 0; i < _host->_ch_data[order].size(); i++) {
    LogicSnapshot::RootNode &rn = _host->_ch_data[order][i];

    for (uint64_t j = 0; j < LogicSnapshot::Scale; j++) {
      uint64_t pos_mask = 1ULL << j;

      if (rn.lbp[j] != nullptr) {
        // Block has actual data — XOR all sample bytes with 0xFF
        uint8_t *lbp = (uint8_t *)rn.lbp[j];
        uint64_t sample_bytes = LogicSnapshot::LeafBlockSamples / 8;

        for (uint64_t k = 0; k < sample_bytes; k++) {
          lbp[k] ^= 0xFF;
        }

        // Rebuild mipmap for this block
        recalc_mipmap(order, i, j);
      } else {
        // Compressed block (constant value) — flip first and last bits
        rn.first ^= pos_mask;
        rn.last ^= pos_mask;
      }
    }
  }

  // Remember that this order is currently inverted. XOR is an involution, so
  // revert_all_edits() undoes the inversion by running it again — but only
  // because this list tells it the inversion is still applied.
  const unsigned int uorder = (unsigned int)order;
  if (std::find(_inverted_orders.begin(), _inverted_orders.end(), uorder) ==
      _inverted_orders.end()) {
    _inverted_orders.push_back(uorder);
  }
}

// ----------------------------------------------------------------------------
// Reversible edit log
// ----------------------------------------------------------------------------

void LogicSnapshotGlitchFilter::record_edit(unsigned int order, uint64_t idx0,
                                            uint64_t idx1, uint64_t byte_lo,
                                            uint64_t byte_hi, bool allocated) {
  // Debug conformance check: this is called immediately before every in-place
  // write of published leaf data, so it is the right place to enforce "all
  // in-place writers hold the exclusive edit transaction". A future writer
  // that forgets EditWriteGuard trips this in debug rather than silently
  // re-opening the torn-revision window (see EditReadGuard).
  assert(_host->edit_write_owned() &&
         "record_edit() outside an EditWriteGuard transaction");

  if (_edit_log_overflow)
    return;

  if (allocated) {
    // The block did not exist (RLE constant representation) and is about to
    // be materialised. Undo = free it again and restore the nullptr plus the
    // constant-value metadata it carried.
    EditRecord e;
    e.order = order;
    e.idx0 = idx0;
    e.idx1 = idx1;
    e.allocated = true;
    e.meta_tog = _host->_ch_data[order][idx0].tog;
    e.meta_first = _host->_ch_data[order][idx0].first;
    e.meta_last = _host->_ch_data[order][idx0].last;
    _edits.push_back(std::move(e));
    if (_edits.size() > kMaxEditRecords)
      _edit_log_overflow = true;
    return;
  }

  const uint64_t data_bytes = LogicSnapshot::LeafBlockSamples / 8;
  if (byte_hi > data_bytes)
    byte_hi = data_bytes;
  if (byte_lo >= byte_hi)
    return;

  void *lbp = _host->_ch_data[order][idx0].lbp[idx1];
  if (lbp == nullptr)
    return;

  const uint64_t len = byte_hi - byte_lo;
  if (_edit_bytes + len > kMaxEditBytes || _edits.size() >= kMaxEditRecords) {
    _edit_log_overflow = true;
    return;
  }

  EditRecord e;
  e.order = order;
  e.idx0 = idx0;
  e.idx1 = idx1;
  e.byte_lo = byte_lo;
  e.allocated = false;
  e.bytes.resize((size_t)len);
  // Snapshot the bytes we are about to clobber. Because records are replayed
  // in reverse, recording per write-run (rather than per block) is already
  // sufficient to restore a block that is rewritten across several batches.
  memcpy(e.bytes.data(), (const uint8_t *)lbp + byte_lo, (size_t)len);
  _edit_bytes += len;
  _edits.push_back(std::move(e));
}

void LogicSnapshotGlitchFilter::clear_edits() {
  _edits.clear();
  _edit_bytes = 0;
  _edit_log_overflow = false;
  _inverted_orders.clear();
}

bool LogicSnapshotGlitchFilter::has_edits() const {
  // The overflow flag is part of the state that revert_all_edits() has to
  // clear: it can be raised on the very first record attempt, before anything
  // is pushed into _edits, and leaving it set would make every later pass
  // refuse to record (see the guard in apply_glitch_filter).
  return !_edits.empty() || !_inverted_orders.empty() || _edit_log_overflow;
}

void LogicSnapshotGlitchFilter::revert_all_edits() {
  std::lock_guard<std::recursive_mutex> lock(_host->_mutex);

  if (!has_edits())
    return;

  // The whole revert rewrites leaf blocks in place, so it needs the same
  // exclusive visibility as a filter batch. Taken once for the entire revert
  // (it is a single logical transaction), which is why the nested
  // invert_channel() call below must NOT take it again.
  LogicSnapshot::EditWriteGuard edit_vis(_host);

  // 1) Replay the log in REVERSE. Ordering note: the inversion was applied
  //    BEFORE the glitch filter, so the logged bytes are post-inversion
  //    content. Restoring the log therefore yields the inverted waveform,
  //    which step 3 then flips back to capture-original.
  for (auto it = _edits.rbegin(); it != _edits.rend(); ++it) {
    const EditRecord &e = *it;
    if (e.order >= _host->_ch_data.size())
      continue;
    if (e.idx0 >= _host->_ch_data[e.order].size())
      continue;

    LogicSnapshot::RootNode &rn = _host->_ch_data[e.order][e.idx0];
    if (e.idx1 >= LogicSnapshot::Scale)
      continue;

    if (e.allocated) {
      void *ptr = rn.lbp[e.idx1];
      rn.lbp[e.idx1] = nullptr;
      // recalc_mipmap() cannot help here (it bails out on a nullptr block),
      // so put back the constant-value representation the block had before
      // it was materialised.
      rn.tog = e.meta_tog;
      rn.first = e.meta_first;
      rn.last = e.meta_last;
      // Deferred only: push_to_free_list() decommits mmap blocks and parks
      // pool blocks in _free_block_list, neither of which makes the storage
      // available to another thread immediately. A lock-free reader that
      // grabbed the pointer before the swap therefore still sees valid
      // memory, and a reader that sees nullptr falls back to the constant
      // first/last representation — which is exactly the pre-edit state.
      if (ptr)
        _host->push_to_free_list(ptr);
    } else {
      void *ptr = rn.lbp[e.idx1];
      if (ptr && !e.bytes.empty())
        memcpy((uint8_t *)ptr + e.byte_lo, e.bytes.data(), e.bytes.size());
    }
  }

  // 2) tog/first/last and the mipmap levels are pure functions of the
  //    restored data, so they are recomputed rather than logged. Ascending
  //    (order, idx0, idx1) order matters: recalc_mipmap() seeds _last_sample
  //    from the preceding block's `last` bit.
  std::vector<std::tuple<unsigned int, uint64_t, uint64_t>> touched;
  touched.reserve(_edits.size());
  for (const EditRecord &e : _edits)
    touched.emplace_back(e.order, e.idx0, e.idx1);
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

  for (const auto &t : touched) {
    if (std::get<0>(t) >= _host->_ch_data.size())
      continue;
    if (std::get<1>(t) >= _host->_ch_data[std::get<0>(t)].size())
      continue;
    recalc_mipmap(std::get<0>(t), std::get<1>(t), std::get<2>(t));
  }

  // Capture-side derived accumulators. They are only meaningful while the
  // capture writes samples; after a revert the mipmaps are authoritative, so
  // reset them for every channel we touched.
  for (const auto &t : touched) {
    const unsigned int order = std::get<0>(t);
    if (order < CHANNEL_MAX_COUNT) {
      _host->_last_calc_count[order] = 0;
      _host->_last_sample[order] = 0;
    }
  }

  // 3) Undo the inversion (involution). Only channels we know are currently
  //    inverted are touched. _inverted_orders must be cleared AFTER the loop:
  //    invert_channel() re-registers the order, and leaving it registered
  //    would make the next revert un-invert already-raw data (i.e. invert it).
  const std::vector<unsigned int> inverted = _inverted_orders;
  _edits.clear();
  _edit_bytes = 0;
  _edit_log_overflow = false;
  _inverted_orders.clear();

  for (unsigned int order : inverted) {
    if (order < _host->_ch_index.size())
      invert_channel(_host->_ch_index[order]);
  }
  _inverted_orders.clear();

  {
    std::lock_guard<std::mutex> rlk(_ranges_mutex);
    _published_ranges.clear();
  }
  _glitch_filtered = false;
}

void LogicSnapshotGlitchFilter::recalc_mipmap(unsigned int order,
                                              uint64_t index0,
                                              uint64_t index1) {
  void *lbp = _host->_ch_data[order][index0].lbp[index1];

  if (lbp == nullptr)
    return;

  if (index1 > 0) {
    void* prev_ptr = _host->_ch_data[order][index0].lbp[index1 - 1];
    if (prev_ptr != nullptr) {
      uint64_t *prev_lbp = (uint64_t *)prev_ptr;
      _host->_last_sample[order] =
          (prev_lbp[LogicSnapshot::LeafBlockSamples / LogicSnapshot::Scale - 1] &
           LogicSnapshot::MSB)
              ? ~0ULL
              : 0ULL;
    } else {
      bool prev_val =
          (_host->_ch_data[order][index0].last & (1ULL << (index1 - 1))) != 0;
      _host->_last_sample[order] = prev_val ? ~0ULL : 0ULL;
    }
  } else if (index0 > 0) {
    bool prev_val =
        (_host->_ch_data[order][index0 - 1].last & LogicSnapshot::MSB) != 0;
    _host->_last_sample[order] = prev_val ? ~0ULL : 0ULL;
  } else {
    _host->_last_sample[order] = 0;
  }

  memset((uint8_t *)lbp + LogicSnapshot::LeafBlockSamples / 8, 0,
         LogicSnapshot::LeafBlockSpace - LogicSnapshot::LeafBlockSamples / 8);

  _host->_ch_data[order][index0].tog &= ~(1ULL << index1);
  _host->_ch_data[order][index0].first &= ~(1ULL << index1);
  _host->_ch_data[order][index0].last &= ~(1ULL << index1);

  _host->_last_calc_count[order] = 0;

  _host->calc_mipmap(order, index0, index1, LogicSnapshot::LeafBlockSamples, true);
}

// ----------------------------------------------------------------------------
// apply_glitch_filter / apply_glitch_filter_all
// ----------------------------------------------------------------------------

void LogicSnapshotGlitchFilter::apply_glitch_filter(
    int sig_index, uint32_t threshold,
    std::function<void(int)> progress_callback,
    GlitchFilterMode filter_mode,
    const std::atomic<bool> *cancel) {
  if (threshold == 0)
    return;

  int order = _host->get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _host->_ch_data.size())
    return;

  uint64_t max_sample = _host->_ring_sample_count;
  if (max_sample == 0)
    return;

  // A previous pass exhausted the edit-log budget and has not been reverted
  // yet. Recording is disabled in that state, so running would modify data
  // we could no longer undo — refuse instead.
  if (_edit_log_overflow) {
    pxv_warn("[GlitchFilter] edit log budget exhausted by an un-reverted "
             "pass; refusing to filter sig_index=%d", sig_index);
    return;
  }

  // Locking policy. Two situations still require locking the whole run:
  //
  //   - loop/∞ mode: blocks are rotated and freed and _loop_offset rebases
  //     the coordinate space, so the scan itself races with readers.
  //   - the capture is still running (!last_ended()): the async writer thread
  //     calls append_payload_impl() under _mutex, and the scan must not read
  //     a leaf block that is being written.
  //
  // Neither applies to the case this refactor targets — filtering a finished
  // finite capture, where every reader on that path
  // (get_display_edges / get_nxt_edge / get_pre_edge / get_sample /
  // get_samples) already bypasses _mutex and synchronizes on
  // committed_sample_count(). There, holding the lock from this line to the
  // `_ring_sample_count` restore at the bottom is what froze the GUI: the
  // window manager declared the app "not responding" because paintEvent()
  // waited for the entire pass. The write phase (apply_batch) now takes the
  // lock for its own duration only, so render frames interleave between
  // batches.
  const bool need_whole_run_lock = _host->_is_loop || !_host->last_ended();
  std::unique_lock<std::recursive_mutex> lock(_host->_mutex, std::defer_lock);
  if (need_whole_run_lock)
    lock.lock();

  // 转换为绝对偏移坐标系
  _host->_ring_sample_count += _host->_loop_offset;

  uint64_t end_pos = max_sample + _host->_loop_offset;
  uint64_t scan_pos = _host->_loop_offset;

  // 状态机记录当前确认的"稳定"电平状态
  bool accepted_level = _host->get_sample_self(scan_pos, sig_index);
  int last_progress = -1;

  pxv_info("[GlitchFilter] START sig_index=%d threshold=%u max_sample=%llu "
           "accepted_level=%d filter_mode=%d",
           sig_index, threshold, (unsigned long long)max_sample,
           accepted_level, (int)filter_mode);

  // 本次滤波的持久化区间在本地累积，全部完成后一次性发布为不可变快照。
  // 渲染线程通过 shared_ptr 拿到的是完整表，永远不会看到 push_back 中途
  // 扩容替换掉的旧 vector（旧实现返回 const& 且不加锁，扩容瞬间渲染线程
  // 手里的迭代器就是悬垂指针）。
  std::vector<LogicSnapshot::FillRange> ranges;
  // FillRange 已提升为 public 嵌套类型 LogicSnapshot::FillRange
  std::vector<LogicSnapshot::FillRange> fills;
  // 预分配批处理空间，防止频繁申请内存
  fills.reserve(65536);
  ranges.reserve(fills.capacity() / 4);

  uint64_t loop_count = 0;
  uint64_t glitch_count = 0;
  uint64_t stable_count = 0;
  // Leaf blocks actually written by the current batch, as (idx0, idx1).
  // Hoisted out of the lambda so the hot path does not reallocate per batch.
  std::vector<std::pair<uint64_t, uint64_t>> dirty;

  // 批量应用覆盖并重构 Mipmap（保证寻找下一边缘时，搜索树不失效）
  auto apply_batch = [&]() {
    if (fills.empty())
      return;

    uint64_t batch_start = fills.front().start;
    uint64_t batch_end = fills.back().end;

    // 只在写阶段取锁；loop 模式下调用方已整趟持有，这里不再重复获取。
    std::unique_lock<std::recursive_mutex> batch_lock(_host->_mutex,
                                                      std::defer_lock);
    if (!lock.owns_lock())
      batch_lock.lock();

    // 编辑可见性事务，作用域 = 本批。有限采集下的渲染读入口
    // (get_display_edges / get_nxt_edge / get_pre_edge / get_sample /
    // pattern_search) 通过 EditReadGuard/consistent_read 与此互斥，因此读者
    // 最多等一个 batch，而不是整趟滤波；同时也不会读到"tog 已清但字节只写了
    // 一半"的中间态。锁序：_mutex -> _edit_visibility（与 revert_all_edits 一致）。
    LogicSnapshot::EditWriteGuard edit_vis(_host);

    pxv_info(
        "[GlitchFilter] apply_batch fills=%zu batch_start=%llu batch_end=%llu",
        fills.size(), (unsigned long long)batch_start,
        (unsigned long long)batch_end);

    for (const auto &r : fills) {
      uint64_t start = r.start;
      uint64_t end = r.end;
      bool level = r.level;

      for (uint64_t pos = start; pos < end;) {
        uint64_t idx0 = pos >> (LogicSnapshot::LeafBlockPower +
                                LogicSnapshot::RootScalePower);
        uint64_t idx1 =
            (pos & LogicSnapshot::RootMask) >> LogicSnapshot::LeafBlockPower;

        if (idx0 >= _host->_ch_data[order].size())
          break;

        uint64_t block_start =
            (idx0 << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower)) +
            (idx1 << LogicSnapshot::LeafBlockPower);
        uint64_t block_end = block_start + LogicSnapshot::LeafBlockSamples;
        uint64_t seg_end = min(end, block_end);

        // 如果该块尚未被实例化，则分配空间
        bool materialised = false;
        if (_host->_ch_data[order][idx0].lbp[idx1] == nullptr) {
          bool const_val =
              (_host->_ch_data[order][idx0].first & (1ULL << idx1)) != 0;
          void *lbp = LeafBlockPool::instance().acquire(LogicSnapshot::LeafBlockSpace);
          if (lbp == nullptr) {
            _host->_memory_failed = true;
            return;
          }
          if (const_val)
            memset(lbp, 0xFF, LogicSnapshot::LeafBlockSamples / 8);
          else
            memset(lbp, 0, LogicSnapshot::LeafBlockSamples / 8);
          memset((uint8_t *)lbp + LogicSnapshot::LeafBlockSamples / 8, 0,
                 LogicSnapshot::LeafBlockSpace -
                     LogicSnapshot::LeafBlockSamples / 8);
          _host->_ch_data[order][idx0].lbp[idx1] = lbp;
          materialised = true;
        }

        dirty.emplace_back(idx0, idx1);

        // Write-ahead bookkeeping, BEFORE the bytes below are clobbered:
        // save the original content of the byte window this run covers. This
        // is what makes the filter reversible without a second full copy of
        // the sample store. record_edit() must stay ahead of the `tog` clear
        // and the bit writes so it captures the pre-edit state.
        {
          const uint64_t seg_lo_bit = pos & LogicSnapshot::LeafMask;
          const uint64_t seg_hi_bit = (seg_end - 1) & LogicSnapshot::LeafMask;
          record_edit(order, idx0, idx1, seg_lo_bit / 8,
                      seg_hi_bit / 8 + 1, materialised);
          if (_edit_log_overflow) {
            // Budget exceeded — stop writing instead of leaving a
            // half-applied pass. The caller reverts and reports a failure.
            pxv_err("[GlitchFilter] edit log budget exceeded at idx0=%llu "
                    "idx1=%llu; aborting so the pass stays reversible",
                    (unsigned long long)idx0, (unsigned long long)idx1);
            return;
          }
        }

        uint8_t *lbp = (uint8_t *)_host->_ch_data[order][idx0].lbp[idx1];

        // 由于马上要改写内容，此处清除该块的跳变标志位
        _host->_ch_data[order][idx0].tog &= ~(1ULL << idx1);

        for (uint64_t i = pos; i < seg_end; i++) {
          uint64_t bit_offset = i & LogicSnapshot::LeafMask;
          uint64_t byte_offset = bit_offset / 8;
          uint8_t bit_mask = 1ULL << (bit_offset % 8);
          if (level)
            lbp[byte_offset] |= bit_mask;
          else
            lbp[byte_offset] &= ~bit_mask;
        }

        pos = seg_end;
      }
    }

    // 精准回写：只重新计算**真正被写过**的叶子块的 Mipmap。
    //
    // 旧实现从 batch 的样本区间反推 [start_blk, end_blk)，然后对区间内每一个
    // 块都做整块重建（memset 33 KB + calc_mipmap 重算 16.7 M 采样的全部
    // mipmap 层）。一批跨越 1000 个块就是 1000 次全块重算 —— 而实际被改写的
    // 块通常是个位数（毛刺是稀疏的）。这里改为只重算写入循环里记录过的块，
    // 把这块开销从 O(块区间) 降到 O(实际改动)。
    std::sort(dirty.begin(), dirty.end());
    dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());

    for (const auto &blk : dirty) {
      // Honour a boundary's cancellation request mid-batch too. A single batch
      // can touch hundreds of leaf blocks and rebuild each block's full mipmap
      // (16.7 M samples), i.e. seconds of work; polling only in the scan loop
      // above would delay a capture/config boundary by that much, which is
      // exactly the stall the cooperative cancel exists to remove.
      //
      // Breaking here leaves the mipmaps partially rebuilt, which is fine: the
      // caller detects the cancel and calls revert_all_edits(), which recomputes
      // every touched block's mipmap from the restored data.
      if (cancel && cancel->load(std::memory_order_relaxed)) {
        pxv_info("[GlitchFilter] cancelled mid-batch at idx0=%llu idx1=%llu",
                 (unsigned long long)blk.first,
                 (unsigned long long)blk.second);
        break;
      }
      if (blk.first < _host->_ch_data[order].size()) {
        recalc_mipmap(order, blk.first, blk.second);
      }
    }
    dirty.clear();

    fills.clear();
  };

  while (scan_pos < end_pos) {
    // Cooperative cancellation, polled once per scan iteration (a relaxed
    // atomic load, i.e. free next to the block reads it sits between).
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      pxv_info("[GlitchFilter] cancelled at scan_pos=%llu; stopping pass",
               (unsigned long long)scan_pos);
      break;
    }

    bool current_scan_level = _host->get_sample_self(scan_pos, sig_index);

    // 寻找下一个边缘（跳出当前电平）— P5 diff+ctz raw 扫描 (spec 阶段3):
    // 直接扫 raw 块字节定位跳变, 不做 mipmap 树遍历, 稠密数据吞吐更高.
    uint64_t edge_pos = scan_pos;
    bool found = _host->find_first_different_raw(
        order, scan_pos, end_pos - 1, current_scan_level, edge_pos);

    // 防死循环: edge_pos 未严格前进说明调用方电平不变式被破坏 (理论上不可达).
    if (!found || edge_pos <= scan_pos) {
      pxv_warn("[GlitchFilter] no valid edge at scan_pos=%llu (raw scan)",
               (unsigned long long)scan_pos);
      break;
    }

    uint64_t pulse_start = edge_pos;
    uint64_t pulse_end = pulse_start;
    // 寻找脉冲的结束边缘（电平回归原始位置）
    bool found_end = _host->find_first_different_raw(
        order, pulse_start, end_pos - 1, !current_scan_level, pulse_end);

    if (!found_end) {
      pulse_end = end_pos;
    }

    uint64_t pulse_len = pulse_end - pulse_start;
    loop_count++;

    if (current_scan_level == accepted_level) {
      if (pulse_len <= threshold) {
        bool should_filter = false;
        switch (filter_mode) {
        case GlitchFilterMode::Both:
          should_filter = true;
          break;
        case GlitchFilterMode::High:
          // Only filter when accepted_level is HIGH (remove low pulses on high level)
          should_filter = accepted_level == true;
          break;
        case GlitchFilterMode::Low:
          // Only filter when accepted_level is LOW (remove high pulses on low level)
          should_filter = accepted_level == false;
          break;
        }

        if (should_filter) {
          // 判断为毛刺：它是一个短暂偏离基准 accepted_level 的窄脉冲
          // 用 accepted_level 覆盖这段毛刺区间
          fills.push_back({pulse_start, pulse_end, accepted_level});
          // apply_batch 仅清空局部 fills；本地 ranges 持续累积，函数结束时
          // 一次性发布。扫描是升序的，因此 ranges 天然按 start 有序，渲染端
          // 可以二分定位可见窗口。
          ranges.push_back({pulse_start, pulse_end, accepted_level});
          glitch_count++;

          if (glitch_count <= 5 || glitch_count % 1000 == 0) {
            pxv_info(
                "[GlitchFilter] GLITCH #%llu scan=%llu pulse=[%llu,%llu) "
                "len=%llu accepted=%d fills=%zu",
                (unsigned long long)glitch_count, (unsigned long long)scan_pos,
                (unsigned long long)pulse_start, (unsigned long long)pulse_end,
                (unsigned long long)pulse_len, accepted_level, fills.size());
          }

          // 跳过毛刺段，由于脉冲结束时恢复到了
          // accepted_level，直接从脉冲末尾继续扫描
          scan_pos = pulse_end;

          // 若堆积过多则刷入硬盘缓存及重建 Mipmap，避免占用过多内存
          if (fills.size() >= 65536) {
            apply_batch();
            if (_host->_memory_failed || _edit_log_overflow)
              break;
          }
        } else {
          // Not filtering this pulse, treat as stable transition.
          // Throttled: a dense-edge signal produces one of these per pulse,
          // and logging every one of them (string formatting + file I/O)
          // used to dominate the pass.
          stable_count++;
          if (stable_count <= 5 || stable_count % 1000 == 0) {
            pxv_info("[GlitchFilter] SKIP-FILTER #%llu scan=%llu pulse=[%llu,%llu) "
                     "len=%llu old_accepted=%d -> new_accepted=%d (mode=%d)",
                     (unsigned long long)stable_count, (unsigned long long)scan_pos,
                     (unsigned long long)pulse_start, (unsigned long long)pulse_end,
                     (unsigned long long)pulse_len, accepted_level,
                     !accepted_level, (int)filter_mode);
          }
          accepted_level = !accepted_level;
          scan_pos = pulse_start;
        }
      } else {
        // 判断为稳定的状态迁移：新电平持续了足够长的时间（同样节流）
        stable_count++;
        if (stable_count <= 5 || stable_count % 1000 == 0) {
          pxv_info("[GlitchFilter] STABLE #%llu scan=%llu pulse=[%llu,%llu) "
                   "len=%llu old_accepted=%d -> new_accepted=%d",
                   (unsigned long long)stable_count, (unsigned long long)scan_pos,
                   (unsigned long long)pulse_start, (unsigned long long)pulse_end,
                   (unsigned long long)pulse_len, accepted_level,
                   !accepted_level);
        }
        accepted_level = !accepted_level; // 确认新的基准电平状态
        scan_pos =
            pulse_start; // 将游标设于稳定脉冲开始处，在下一次循环中作为新基准点搜索
      }
    } else {
      // 防御性设计：依照状态机逻辑不会跑到这
      pxv_warn("[GlitchFilter] UNEXPECTED current_scan_level(%d) != "
               "accepted_level(%d) at scan_pos=%llu",
               current_scan_level, accepted_level,
               (unsigned long long)scan_pos);
      scan_pos = pulse_start;
    }

    int progress = (int)((scan_pos - _host->_loop_offset) * 100 / max_sample);
    if (progress != last_progress && progress_callback) {
      progress_callback(progress);
      last_progress = progress;
    }
  }

  // 处理遗留的一批写操作
  apply_batch();

  // 验证：采样前100个点，确认数据确实被修改了（仅在失配时输出）
  for (int v = 0; v < 100; v++) {
    uint64_t vpos = _host->_loop_offset + v;
    bool vlevel = _host->get_sample_self(vpos, sig_index);
    if (vlevel != accepted_level) {
      pxv_info(
          "[GlitchFilter] VERIFY pos=%llu level=%d (MISMATCH! expected=%d)",
          (unsigned long long)vpos, vlevel, accepted_level);
    }
  }
  for (size_t fi = 0; fi < ranges.size() && fi < 5; fi++) {
    uint64_t vpos = ranges[fi].start;
    bool vlevel = _host->get_sample_self(vpos, sig_index);
    pxv_info("[GlitchFilter] VERIFY fill[%zu] start_pos=%llu level=%d expected=%d",
             fi, (unsigned long long)vpos, vlevel, ranges[fi].level);
  }

  pxv_info("[GlitchFilter] END sig_index=%d loops=%llu glitches=%llu "
           "stables=%llu ranges=%zu edits=%zu edit_bytes=%llu",
           sig_index, (unsigned long long)loop_count,
           (unsigned long long)glitch_count, (unsigned long long)stable_count,
           ranges.size(), _edits.size(), (unsigned long long)_edit_bytes);

  // 发布不可变区间表（换入一个新 shared_ptr，读者不会被阻塞）。
  // 预算超限或 OOM 时整趟会被调用方 revert，这里不发布，避免渲染端看到半截区间。
  if (!_edit_log_overflow && !_host->_memory_failed) {
    auto table = std::make_shared<std::vector<LogicSnapshot::FillRange>>(
        std::move(ranges));
    std::lock_guard<std::mutex> rlk(_ranges_mutex);
    _published_ranges[sig_index] = std::move(table);
  }

  // 恢复坐标系
  _host->_ring_sample_count -= _host->_loop_offset;
}

void LogicSnapshotGlitchFilter::apply_glitch_filter_all(
    const std::map<int, uint32_t> &thresholds,
    std::function<void(int)> progress_callback,
    const std::map<int, GlitchFilterMode> &filter_modes,
    const std::atomic<bool> *cancel) {
  // 架构修复：按 channel_index 查找阈值，与 _ch_index 中的位置无关
  for (size_t i = 0; i < _host->_ch_index.size(); i++) {
    if (cancel && cancel->load(std::memory_order_relaxed))
      return;
    int ch_idx = _host->_ch_index[i];
    auto it = thresholds.find(ch_idx);
    if (it != thresholds.end() && it->second > 0) {
      GlitchFilterMode mode = GlitchFilterMode::Both;
      auto mit = filter_modes.find(ch_idx);
      if (mit != filter_modes.end())
        mode = mit->second;
      apply_glitch_filter(ch_idx, it->second, nullptr, mode, cancel);
      // 失败/超预算时停止遍历：调用方会整趟 revert 并上报失败，
      // 继续滤后面的通道只会扩大需要回滚的范围。
      if (_edit_log_overflow || _host->_memory_failed)
        return;
    }
    if (progress_callback) {
      int progress = (int)((i + 1) * 100 / _host->_ch_index.size());
      progress_callback(progress);
    }
  }
  _glitch_filtered = true;
}

// ----------------------------------------------------------------------------
// 纯单通道毛刺滤波（决策逻辑）— 与 apply_glitch_filter 状态机逐分支等价，
// 但对单通道字节流直接运算、零拷贝输入，抽离成可独立单测的纯函数。
// ----------------------------------------------------------------------------
void apply_glitch_filter_one_pass(const uint8_t *in, uint8_t *out,
                                  uint64_t sample_count, uint32_t threshold,
                                  GlitchFilterMode filter_mode) {
  if (in == nullptr || out == nullptr || sample_count == 0 || threshold == 0)
    return;

  // 先用输入填充输出，未判定为毛刺的样本保持原电平
  memcpy(out, in, (size_t)sample_count);

  bool accepted_level = in[0] != 0;
  uint64_t scan_pos = 0;
  const uint64_t end_pos = sample_count;

  while (scan_pos < end_pos) {
    const bool current_scan_level = in[scan_pos] != 0;

    // 寻找下一个边缘：第一个电平 != current 的位置
    uint64_t edge_pos = scan_pos;
    while (edge_pos < end_pos && (in[edge_pos] != 0) == current_scan_level)
      ++edge_pos;
    if (edge_pos >= end_pos)
      break;  // 无更多边缘

    // 从边缘起量取脉冲段 [pulse_start, pulse_end)，其电平 == !current
    uint64_t pulse_start = edge_pos;
    uint64_t pulse_end = pulse_start;
    while (pulse_end < end_pos && (in[pulse_end] != 0) != current_scan_level)
      ++pulse_end;
    const uint64_t pulse_len = pulse_end - pulse_start;

    if (current_scan_level == accepted_level) {
      if (pulse_len <= threshold) {
        bool should_filter = false;
        switch (filter_mode) {
        case GlitchFilterMode::Both:
          should_filter = true;
          break;
        case GlitchFilterMode::High:
          // 仅在基准电平为高时滤除其上的窄低凹
          should_filter = (accepted_level == true);
          break;
        case GlitchFilterMode::Low:
          // 仅在基准电平为低时滤除其上的窄高刺
          should_filter = (accepted_level == false);
          break;
        }
        if (should_filter) {
          // 判定为毛刺：用稳定基准电平覆盖该段
          for (uint64_t p = pulse_start; p < pulse_end; ++p)
            out[p] = accepted_level ? 1 : 0;
          scan_pos = pulse_end;
        } else {
          // 该模式不滤，作为稳定迁移处理
          accepted_level = !accepted_level;
          scan_pos = pulse_start;
        }
      } else {
        // 宽脉冲：确认新的基准电平
        accepted_level = !accepted_level;
        scan_pos = pulse_start;
      }
    } else {
      // 防御性分支，正常状态机不会进入
      scan_pos = pulse_start;
    }
  }
}

// ----------------------------------------------------------------------------
// State / persisted-range accessors
// ----------------------------------------------------------------------------

bool LogicSnapshotGlitchFilter::is_glitch_filtered() const {
  return _glitch_filtered;
}

void LogicSnapshotGlitchFilter::set_glitch_filtered(bool filtered) {
  _glitch_filtered = filtered;
}

std::shared_ptr<const std::vector<LogicSnapshot::FillRange>>
LogicSnapshotGlitchFilter::get_filtered_ranges(int sig_index) const {
  // Copy the shared_ptr out under a lock held for exactly that long. The
  // pointed-to table is immutable, so the caller can walk it without any
  // synchronization (and without a reallocation pulling the rug from under
  // it — the previous API returned a `const&` into the worker's live vector).
  std::lock_guard<std::mutex> lk(_ranges_mutex);
  auto it = _published_ranges.find(sig_index);
  if (it == _published_ranges.end())
    return nullptr;
  return it->second;
}

void LogicSnapshotGlitchFilter::clear_filtered_ranges() {
  std::lock_guard<std::mutex> lk(_ranges_mutex);
  _published_ranges.clear();
}

}  // namespace data
}  // namespace pv
