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
#include <cstring>

#include "../pxvdef.h"
#include "../log.h"
#include "leaf_block_pool.h"
#include "logicsnapshot_glitch_filter.h"

using namespace std;

namespace pv {
namespace data {

const std::vector<LogicSnapshot::FillRange>
    LogicSnapshotGlitchFilter::_empty_filtered_ranges;

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

void LogicSnapshotGlitchFilter::invert_channel(int sig_index) {
  std::lock_guard<std::mutex> lock(_host->_mutex);

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
    GlitchFilterMode filter_mode) {
  if (threshold == 0)
    return;

  int order = _host->get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _host->_ch_data.size())
    return;

  uint64_t max_sample = _host->_ring_sample_count;
  if (max_sample == 0)
    return;

  std::lock_guard<std::mutex> lock(_host->_mutex);

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

  // 重新滤波从空白开始累积持久化区间，防止重复累积
  _filtered_ranges_per_channel[sig_index].clear();

  // FillRange 已提升为 public 嵌套类型 LogicSnapshot::FillRange
  std::vector<LogicSnapshot::FillRange> fills;
  // 预分配批处理空间，防止频繁申请内存
  fills.reserve(65536);

  uint64_t loop_count = 0;
  uint64_t glitch_count = 0;
  uint64_t stable_count = 0;

  // 批量应用覆盖并重构 Mipmap（保证寻找下一边缘时，搜索树不失效）
  auto apply_batch = [&]() {
    if (fills.empty())
      return;

    uint64_t batch_start = fills.front().start;
    uint64_t batch_end = fills.back().end;

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

    // 精准回写：只重新计算被改写过的叶子节点的 Mipmap
    // 从而完美维护查找树的同步，且大量节省 CPU 耗时
    uint64_t start_blk = batch_start / LogicSnapshot::LeafBlockSamples;
    uint64_t end_blk =
        (batch_end + LogicSnapshot::LeafBlockSamples - 1) /
        LogicSnapshot::LeafBlockSamples;

    for (uint64_t blk = start_blk; blk < end_blk; ++blk) {
      uint64_t idx0 = blk / LogicSnapshot::RootScale;
      uint64_t idx1 = blk % LogicSnapshot::RootScale;
      if (idx0 < _host->_ch_data[order].size()) {
        recalc_mipmap(order, idx0, idx1);
      }
    }

    fills.clear();
  };

  while (scan_pos < end_pos) {
    bool current_scan_level = _host->get_sample_self(scan_pos, sig_index);

    // 寻找下一个边缘（跳出当前电平）
    uint64_t edge_pos = scan_pos;
    bool found = _host->get_nxt_edge_self(edge_pos, current_scan_level,
                                          end_pos - 1, 0, sig_index);

    if (!found) {
      pxv_info("[GlitchFilter] no more edges at scan_pos=%llu",
               (unsigned long long)scan_pos);
      break;
    }

    uint64_t pulse_start = edge_pos;
    uint64_t pulse_end = pulse_start;
    // 寻找脉冲的结束边缘（电平回归原始位置）
    bool found_end = _host->get_nxt_edge_self(pulse_end, !current_scan_level,
                                               end_pos - 1, 0, sig_index);

    if (!found_end) {
      pulse_end = end_pos;
    }

    uint64_t pulse_len = pulse_end - pulse_start;
    loop_count++;

    if (current_scan_level == accepted_level) {
      if (pulse_len <= threshold) {
        bool should_filter = false;
        switch (filter_mode) {
        case GLITCH_FILTER_BOTH:
          should_filter = true;
          break;
        case GLITCH_FILTER_HIGH:
          // Only filter when accepted_level is HIGH (remove low pulses on high level)
          should_filter = accepted_level == true;
          break;
        case GLITCH_FILTER_LOW:
          // Only filter when accepted_level is LOW (remove high pulses on low level)
          should_filter = accepted_level == false;
          break;
        }

        if (should_filter) {
          // 判断为毛刺：它是一个短暂偏离基准 accepted_level 的窄脉冲
          // 用 accepted_level 覆盖这段毛刺区间
          fills.push_back({pulse_start, pulse_end, accepted_level});
          // 同步写入持久化区间（apply_batch 仅清空局部 fills，持久化存储累积保留）
          _filtered_ranges_per_channel[sig_index].push_back(
              {pulse_start, pulse_end, accepted_level});
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
            if (_host->_memory_failed)
              break;
          }
        } else {
          // Not filtering this pulse, treat as stable transition
          stable_count++;
          pxv_info("[GlitchFilter] SKIP-FILTER #%llu scan=%llu pulse=[%llu,%llu) "
                   "len=%llu old_accepted=%d -> new_accepted=%d (mode=%d)",
                   (unsigned long long)stable_count, (unsigned long long)scan_pos,
                   (unsigned long long)pulse_start, (unsigned long long)pulse_end,
                   (unsigned long long)pulse_len, accepted_level,
                   !accepted_level, (int)filter_mode);
          accepted_level = !accepted_level;
          scan_pos = pulse_start;
        }
      } else {
        // 判断为稳定的状态迁移：新电平持续了足够长的时间
        stable_count++;
        pxv_info("[GlitchFilter] STABLE #%llu scan=%llu pulse=[%llu,%llu) "
                 "len=%llu old_accepted=%d -> new_accepted=%d",
                 (unsigned long long)stable_count, (unsigned long long)scan_pos,
                 (unsigned long long)pulse_start, (unsigned long long)pulse_end,
                 (unsigned long long)pulse_len, accepted_level,
                 !accepted_level);
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

  // 验证：采样前100个点，确认数据确实被修改了
  pxv_info(
      "[GlitchFilter] VERIFY start: sampling first 100 points after filter");
  for (int v = 0; v < 100; v++) {
    uint64_t vpos = _host->_loop_offset + v;
    bool vlevel = _host->get_sample_self(vpos, sig_index);
    if (vlevel != accepted_level) {
      pxv_info(
          "[GlitchFilter] VERIFY pos=%llu level=%d (MISMATCH! expected=%d)",
          (unsigned long long)vpos, vlevel, accepted_level);
    }
  }
  pxv_info("[GlitchFilter] VERIFY: also sampling fills region boundaries");
  if (!fills.empty()) {
    for (size_t fi = 0; fi < fills.size() && fi < 5; fi++) {
      uint64_t vpos = fills[fi].start;
      bool vlevel = _host->get_sample_self(vpos, sig_index);
      pxv_info(
          "[GlitchFilter] VERIFY fill[%zu] start_pos=%llu level=%d expected=%d",
          fi, (unsigned long long)vpos, vlevel, fills[fi].level);
    }
  }

  pxv_info("[GlitchFilter] END sig_index=%d loops=%llu glitches=%llu "
           "stables=%llu fills_final=%zu",
           sig_index, (unsigned long long)loop_count,
           (unsigned long long)glitch_count, (unsigned long long)stable_count,
           fills.size());

  // 恢复坐标系
  _host->_ring_sample_count -= _host->_loop_offset;
}

void LogicSnapshotGlitchFilter::apply_glitch_filter_all(
    const std::map<int, uint32_t> &thresholds,
    std::function<void(int)> progress_callback,
    const std::map<int, GlitchFilterMode> &filter_modes) {
  // 架构修复：按 channel_index 查找阈值，与 _ch_index 中的位置无关
  for (size_t i = 0; i < _host->_ch_index.size(); i++) {
    int ch_idx = _host->_ch_index[i];
    auto it = thresholds.find(ch_idx);
    if (it != thresholds.end() && it->second > 0) {
      GlitchFilterMode mode = GLITCH_FILTER_BOTH;
      auto mit = filter_modes.find(ch_idx);
      if (mit != filter_modes.end())
        mode = mit->second;
      apply_glitch_filter(ch_idx, it->second, nullptr, mode);
    }
    if (progress_callback) {
      int progress = (int)((i + 1) * 100 / _host->_ch_index.size());
      progress_callback(progress);
    }
  }
  _glitch_filtered = true;
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

const std::vector<LogicSnapshot::FillRange>&
LogicSnapshotGlitchFilter::get_filtered_ranges(int sig_index) const {
  auto it = _filtered_ranges_per_channel.find(sig_index);
  if (it == _filtered_ranges_per_channel.end() || it->second.empty()) {
    return _empty_filtered_ranges;
  }
  return it->second;
}

void LogicSnapshotGlitchFilter::clear_filtered_ranges() {
  std::lock_guard<std::mutex> lock(_host->_mutex);
  _filtered_ranges_per_channel.clear();
}

}  // namespace data
}  // namespace pv
