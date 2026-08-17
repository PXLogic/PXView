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
// logicsnapshot_edge_scan.h) and logicsnapshot_edge_scan.h.
// logicsnapshot.h forward-declares LogicSnapshotEdgeScan and holds it via
// unique_ptr<forward-declared type>; the forwarders live in logicsnapshot.cpp.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

#include "pv/base/log.h"
#include "pv/data/snapshot/logicsnapshot_edge_scan.h"

using namespace std;

namespace pv {
namespace data {

namespace {
/* [RenderDiag] get_display_edges 逐帧耗时统计 (GUI 线程).
 *
 * 定位渲染卡顿 (spec 阶段3 Render 诊断日志体系): 慢调用 (>100ms) 打印带
 * 1s throttle; 每 1000 次汇总 avg/max. RAII 覆盖所有 return 路径. */
struct RenderDiagRec {
    uint64_t start, end;
    uint16_t width;
    inline static unsigned long long calls = 0;
    inline static double total_ms = 0, max_ms = 0;
    std::chrono::steady_clock::time_point t0;
    inline static std::chrono::steady_clock::time_point t_last{};

    RenderDiagRec(uint64_t s, uint64_t e, uint16_t w)
        : start(s), end(e), width(w),
          t0(std::chrono::steady_clock::now()) {}

    ~RenderDiagRec() {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        calls++;
        total_ms += ms;
        if (ms > max_ms) max_ms = ms;
        const auto now = std::chrono::steady_clock::now();
        if (ms >= 100.0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - t_last).count() >= 1000) {
            t_last = now;
            pxv_warn("[RenderDiag] get_display_edges slow: %.1fms "
                     "(start=%llu end=%llu width=%u)",
                     ms, (unsigned long long)start,
                     (unsigned long long)end, (unsigned)width);
        }
        if (calls % 1000 == 0) {
            pxv_info("[RenderDiag] get_display_edges: %llu calls "
                     "avg=%.2fms max=%.1fms",
                     calls, total_ms / 1000.0, max_ms);
            total_ms = 0; max_ms = 0;
        }
    }
};
} // anonymous namespace


// ----------------------------------------------------------------------------
// Construction / destruction
// ----------------------------------------------------------------------------

LogicSnapshotEdgeScan::LogicSnapshotEdgeScan(LogicSnapshot *host)
    : _host(host)
{
}

LogicSnapshotEdgeScan::~LogicSnapshotEdgeScan()
{
}


// ----------------------------------------------------------------------------
// get_display_edges / get_display_edges_common
// ----------------------------------------------------------------------------

bool LogicSnapshotEdgeScan::get_display_edges(
    std::vector<std::pair<bool, bool>> &edges,
    std::vector<std::pair<uint16_t, bool>> &togs, uint64_t start, uint64_t end,
    uint16_t width, uint16_t max_togs, double pixels_offset, double min_length,
    uint16_t sig_index) {
  // [RenderDiag] 逐帧渲染耗时统计 (RAII, 覆盖所有 return 路径)
  RenderDiagRec rd_diag(start, end, width);

  if (!edges.empty())
    edges.clear();
  if (!togs.empty())
    togs.clear();

  // C3 (P9-on-raw): FINITE (non-loop) fast path is lock-free. Reads only the
  // committed sample range (`_ring_published`, release-published after mipmap
  // completes), so it never races with in-place mipmap metadata mutation or
  // block writes. `_loop_offset` is 0 for finite captures — not read here.
  if (!_host->_is_loop) {
    const uint64_t sample_count = _host->committed_sample_count();
    if (sample_count == 0)
      return false;

    // Guard: end/start may be invalid when called during file loading or
    // after a stale config restore. Clamp `end` to the committed range.
    if (end >= sample_count)
      end = sample_count - 1;
    if (start > end || min_length <= 0) {
      pxv_warn("LogicSnapshot::get_display_edges: committed=%llu start=%llu "
               "end=%llu min_len=%f",
               (unsigned long long)sample_count, (unsigned long long)start,
               (unsigned long long)end, min_length);
      return false;
    }

    return get_display_edges_common(edges, togs, start, end, width, max_togs,
                                    pixels_offset, min_length, sig_index,
                                    sample_count);
  }

  std::lock_guard<std::recursive_mutex> lock(_host->_mutex);

  if (_host->_ring_sample_count == 0)
    return false;

  // Guard: end/start may be invalid when called during file loading or
  // after a stale config restore. Original asserts crash via MSVC dialog.
  if (end >= _host->_ring_sample_count || start > end || min_length <= 0) {
    pxv_warn("LogicSnapshot::get_display_edges: ring=%llu start=%llu "
             "end=%llu min_len=%f",
             (unsigned long long)_host->_ring_sample_count, (unsigned long long)start,
             (unsigned long long)end, min_length);
    return false;
  }

  return get_display_edges_common(edges, togs, start, end, width, max_togs,
                                  pixels_offset, min_length, sig_index,
                                  _host->_ring_sample_count);
}

bool LogicSnapshotEdgeScan::get_display_edges_common(
    std::vector<std::pair<bool, bool>> &edges,
    std::vector<std::pair<uint16_t, bool>> &togs, uint64_t start, uint64_t end,
    uint16_t width, uint16_t max_togs, double pixels_offset, double min_length,
    uint16_t sig_index, uint64_t sample_count) {
  (void)sample_count;  // bounds already validated by the caller

  uint64_t index = start;
  bool last_sample;
  bool start_sample;

  // Get the initial state. FINITE path (caller holds no lock): index is
  // physical == user (loop_offset==0), use the `_self` read (lock-free).
  // LOOP path (caller holds _mutex): index is user coords; use the `_unlock`
  // wrapper which rebases by `_loop_offset` and temporarily bumps
  // `_ring_sample_count` so `_self` sees the loop-adjusted committed count.
  start_sample = last_sample = _host->_is_loop ? _host->get_sample_unlock(index++, sig_index)
                                        : _host->get_sample_self(index++, sig_index);
  togs.push_back(pair<uint16_t, bool>(0, last_sample));

  while (edges.size() < width) {
    // search next edge
    bool has_edge = _host->_is_loop
                        ? get_nxt_edge_unlock(index, last_sample, end, 0, sig_index)
                        : get_nxt_edge_self(index, last_sample, end, 0, sig_index);

    // calc the edge position
    int64_t gap = (index / min_length) - pixels_offset;
    index = max((uint64_t)ceil((floor(index / min_length) + 1) * min_length),
                index + 1);

    while (gap > (int64_t)edges.size() && edges.size() < width) {
      edges.push_back(pair<bool, bool>(false, last_sample));
    }

    if (index > end)
      last_sample = _host->_is_loop ? _host->get_sample_unlock(end, sig_index)
                             : _host->get_sample_self(end, sig_index);
    else
      last_sample = _host->_is_loop ? _host->get_sample_unlock(index - 1, sig_index)
                             : _host->get_sample_self(index - 1, sig_index);

    // NOTE: the has_edge push below may bring `edges` to width+1 when the
    // gap loop already filled it to `width` — this is the renderer's intended
    // contract (logicsignal.cpp asserts _cur_pulses.size() >= width and uses
    // the boundary toggle at index `width`), NOT an overflow bug.
    if (has_edge) {
      edges.push_back(pair<bool, bool>(true, last_sample));
      if (togs.size() < max_togs)
        togs.push_back(pair<uint16_t, bool>(edges.size() - 1, last_sample));
    }

    while (index > end && edges.size() < width)
      edges.push_back(pair<bool, bool>(false, last_sample));
  }

  if (togs.size() < max_togs && !edges.empty()) {
    last_sample = _host->_is_loop ? _host->get_sample_unlock(end, sig_index)
                           : _host->get_sample_self(end, sig_index);
    togs.push_back(pair<uint16_t, bool>(edges.size() - 1, last_sample));
  }

  return start_sample;
}


// ----------------------------------------------------------------------------
// get_nxt_edge / get_nxt_edge_unlock / get_nxt_edge_self
// ----------------------------------------------------------------------------

bool LogicSnapshotEdgeScan::get_nxt_edge(uint64_t &index, bool last_sample,
                                 uint64_t end, double min_length,
                                 int sig_index) {
  // C3 (P9-on-raw): FINITE (non-loop) fast path is lock-free — see
  // committed_sample_count() for the publication protocol.
  if (!_host->_is_loop) {
    const uint64_t sample_count = _host->committed_sample_count();
    if (sample_count == 0 || index > end || index >= sample_count)
      return false;
    return get_nxt_edge_self(index, last_sample, end, min_length, sig_index);
  }

  std::lock_guard<std::recursive_mutex> lock(_host->_mutex);
  return get_nxt_edge_unlock(index, last_sample, end, min_length, sig_index);
}

bool LogicSnapshotEdgeScan::get_nxt_edge_unlock(uint64_t &index, bool last_sample,
                                        uint64_t end, double min_length,
                                        int sig_index) {
  index += _host->_loop_offset;
  end += _host->_loop_offset;
  _host->_ring_sample_count += _host->_loop_offset;

  bool flag = get_nxt_edge_self(index, last_sample, end, min_length, sig_index);

  index -= _host->_loop_offset;
  _host->_ring_sample_count -= _host->_loop_offset;

  return flag;
}

bool LogicSnapshotEdgeScan::get_nxt_edge_self(uint64_t &index, bool last_sample,
                                      uint64_t end, double min_length,
                                      int sig_index) {
  if (index > end) {
    pxv_warn("LogicSnapshot::get_nxt_edge_self: index=%llu > end=%llu",
             (unsigned long long)index, (unsigned long long)end);
    return false;
  }

  int order = _host->get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _host->_ch_data.size()) {
    pxv_warn("LogicSnapshot::get_nxt_edge_self: invalid order for sig_index=%d", sig_index);
    return false;
  }

  // const unsigned int min_level = max((int)floorf(logf(min_length) /
  // logf(Scale)) - 1, 0);
  const unsigned int min_level =
      max((int)(log2f(min_length) - 1) / (int)LogicSnapshot::ScalePower, 0);
  uint64_t root_index = index >> (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower);
  uint8_t root_pos = (index & LogicSnapshot::RootMask) >> LogicSnapshot::LeafBlockPower;
  bool root_last = (root_index != 0 && root_index - 1 < _host->_ch_data[order].size())
                       ? _host->_ch_data[order][root_index - 1].last & LogicSnapshot::MSB
                       : _host->_ch_data[order][0].first & LogicSnapshot::LSB;
  bool edge_hit = false;

  // linear search for the next transition on the root level
  for (uint64_t i = root_index;
       !edge_hit && (index <= end) && i < (uint64_t)_host->_ch_data[order].size();
       i++) {
    uint64_t cur_mask = (~0ULL << root_pos);

    do {
      uint64_t inner_tog = _host->_ch_data[order][i].tog & cur_mask;
      uint64_t lbp_tog_raw =
          (((_host->_ch_data[order][i].last << 1) + root_last) & cur_mask) ^
          (_host->_ch_data[order][i].first & cur_mask);
      uint64_t lbp_tog = lbp_tog_raw;

      // 修复：清除超出 committed sample count 的虚假 lbp_tog bit。
      // 未使用的 lbp 块 first/last=0，会与已使用块的 last 产生虚假边沿
      // （例如 D7 全高时，lbp[0] last=1，lbp[1] first=0 → 误报下降沿，
      //  导致 lbp_nxt_edge 把 index 推到 lbp[1] 起始位置 2^24，
      //  渲染端画出垂直线毛刺）。
      // 只有 lbp_tog_index < committed count 的 bit 才是有效边沿。
      // C3: 有限采集下用原子 `_ring_published`（无锁读），loop 下用
      // 调用方临时抬高的 `_ring_sample_count`。
      {
        const uint64_t committed = _host->committed_sample_count();
        uint64_t root_start = (i << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower));
        if (root_start < committed) {
          uint64_t valid_lbp_count = (committed - root_start +
                                      LogicSnapshot::LeafBlockSamples - 1) >> LogicSnapshot::LeafBlockPower;
          uint64_t valid_mask = (valid_lbp_count >= LogicSnapshot::Scale)
                                    ? ~0ULL
                                    : ((1ULL << valid_lbp_count) - 1);
          lbp_tog &= valid_mask;
        } else {
          lbp_tog = 0;
        }
      }

      uint8_t inner_tog_pos = _host->bsf_folded(inner_tog);
      uint8_t lbp_tog_pos = _host->bsf_folded(lbp_tog);

      if (inner_tog != 0) {
        if (lbp_tog != 0) {
          // lbp tog before inner tog
          edge_hit = lbp_nxt_edge(index, i, lbp_tog, lbp_tog_pos, true,
                                  inner_tog_pos, last_sample, sig_index);
        }

        if (!edge_hit) {
          void *ptr = _host->_ch_data[order][i].lbp[inner_tog_pos];
          uint64_t blk_start = (i << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower)) +
                               (inner_tog_pos << LogicSnapshot::LeafBlockPower);
          index = max(blk_start, index);

          if (ptr != nullptr && min_level < LogicSnapshot::ScaleLevel) {
            uint64_t block_end = min(index | LogicSnapshot::LeafMask, end);
            edge_hit =
                block_nxt_edge((uint64_t *)ptr, index, block_end, last_sample, min_level);
          } else if (ptr != nullptr) {
            edge_hit = true;
          } else {
            edge_hit = true; // block unavailable, treat as edge
          }

          if (inner_tog_pos == LogicSnapshot::RootScale - 1)
            break;
          cur_mask = (~0ULL << (inner_tog_pos + 1));
        }
      } else if (lbp_tog != 0) {
        // lbp tog
        edge_hit = lbp_nxt_edge(index, i, lbp_tog, lbp_tog_pos, false,
                                LogicSnapshot::Scale - 1, last_sample, sig_index);
      } else {
        // index = (index + (1 << (LeafBlockPower + RootScalePower))) &
        //         (~0ULL << (LeafBlockPower + RootScalePower));
        index = (((i + 1) << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower)) - 1);
        break;
      }
    }
    // while (!edge_hit && index < end);
    while (!edge_hit &&
           index < (((i + 1) << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower)) - 1));

    root_pos = 0;
    root_last = _host->_ch_data[order][i].last & LogicSnapshot::MSB;
  }

  if (index > end) {
    // skip edges over right
    edge_hit = false;
  }

  // DEBUG: verify edge position
  if (edge_hit && sig_index == 14) {
    bool sample_at_edge = _host->get_sample_self(index, sig_index);
    bool sample_before =
        (index > 0) ? _host->get_sample_self(index - 1, sig_index) : last_sample;
    if (sample_at_edge == sample_before) {
      pxv_warn("[GlitchFilter] FAKE EDGE at %llu: before=%d at=%d (same!)",
               (unsigned long long)index, sample_before, sample_at_edge);
    }
  }

  return edge_hit;
}


// ----------------------------------------------------------------------------
// get_pre_edge / get_pre_edge_self
// ----------------------------------------------------------------------------

bool LogicSnapshotEdgeScan::get_pre_edge(uint64_t &index, bool last_sample,
                                 double min_length, int sig_index) {
  // C3 (P9-on-raw): FINITE (non-loop) fast path is lock-free — see
  // committed_sample_count() for the publication protocol.
  if (!_host->_is_loop) {
    const uint64_t sample_count = _host->committed_sample_count();
    if (sample_count == 0 || index >= sample_count)
      return false;
    return get_pre_edge_self(index, last_sample, min_length, sig_index);
  }

  std::lock_guard<std::recursive_mutex> lock(_host->_mutex);

  index += _host->_loop_offset;
  _host->_ring_sample_count += _host->_loop_offset;

  bool flag = get_pre_edge_self(index, last_sample, min_length, sig_index);

  index = (index < _host->_loop_offset) ? 0 : index - _host->_loop_offset;
  _host->_ring_sample_count -= _host->_loop_offset;
  return flag;
}

bool LogicSnapshotEdgeScan::get_pre_edge_self(uint64_t &index, bool last_sample,
                                      double min_length, int sig_index) {
  // Guard: when opening a saved waveform file, the committed sample count may
  // be 0 (data not fully loaded or race condition during file open). The
  // original assert(index < _ring_sample_count) crashes via the MSVC assertion
  // dialog. Replace with a safe bounds check that returns false.
  // C3: 有限采集下用原子 `_ring_published`（无锁读），loop 下用调用方临时
  // 抬高的 `_ring_sample_count`。
  const uint64_t committed = _host->committed_sample_count();
  if (committed == 0 || index >= committed) {
    pxv_warn("LogicSnapshot::get_pre_edge_self: index=%llu committed=%llu, skipping",
             (unsigned long long)index, (unsigned long long)committed);
    return false;
  }

  int order = _host->get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _host->_ch_data.size()) {
    pxv_warn("LogicSnapshot::get_pre_edge_self: invalid order for sig_index=%d", sig_index);
    return false;
  }
  // logf(Scale)) - 1, 1);
  const unsigned int min_level =
      max((int)(log2f(min_length) - 1) / (int)LogicSnapshot::ScalePower, 0);
  int root_index = index >> (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower);
  uint8_t root_pos = (index & LogicSnapshot::RootMask) >> LogicSnapshot::LeafBlockPower;
  if ((unsigned int)root_index >= _host->_ch_data[order].size()) {
    pxv_warn("LogicSnapshot::get_pre_edge_self: root_index=%llu out of range (size=%zu)",
             (unsigned long long)root_index, _host->_ch_data[order].size());
    return false;
  }
  bool root_first = _host->_ch_data[order][root_index].last & LogicSnapshot::MSB;
  bool edge_hit = false;

  // linear search for the previous transition on the root level
  for (int64_t i = root_index; !edge_hit && i >= 0; i--) {
    uint64_t cur_mask = (~0ULL >> (LogicSnapshot::RootScale - root_pos - 1));

    do {
      uint64_t inner_tog = _host->_ch_data[order][i].tog & cur_mask;
      uint64_t lbp_tog = (_host->_ch_data[order][i].last & cur_mask) ^
                         ((((uint64_t)root_first << (LogicSnapshot::RootScale - 1)) +
                           (_host->_ch_data[order][i].first >> 1)) &
                          cur_mask);
      uint8_t inner_tog_pos = _host->bsr64(inner_tog);
      uint8_t lbp_tog_pos = _host->bsr64(lbp_tog);

      if (inner_tog != 0) {
        if (lbp_tog != 0) {
          // lbp tog before inner tog
          edge_hit = lbp_pre_edge(index, i, lbp_tog, lbp_tog_pos, true,
                                  inner_tog_pos, last_sample, sig_index);
        }

        if (!edge_hit) {
          void *ptr = _host->_ch_data[order][i].lbp[inner_tog_pos];
          uint64_t blk_end = ((i << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower)) +
                              (inner_tog_pos << LogicSnapshot::LeafBlockPower)) |
                             LogicSnapshot::LeafMask;
          index = min(blk_end, index);
          if (ptr != nullptr && min_level < LogicSnapshot::ScaleLevel) {
            edge_hit =
                block_pre_edge((uint64_t *)ptr, index, last_sample, min_level, sig_index);
          } else {
            edge_hit = true;
          }
          if (inner_tog_pos == 0)
            break;
          cur_mask = (~0ULL >> (LogicSnapshot::RootScale - inner_tog_pos));
        }
      } else if (lbp_tog != 0) {
        // lbp tog
        edge_hit = lbp_pre_edge(index, i, lbp_tog, lbp_tog_pos, false, 0,
                                last_sample, sig_index);
        if (lbp_tog_pos == 0)
          break;
      } else {
        break;
      }
    } while (!edge_hit);

    root_pos = LogicSnapshot::RootScale - 1;
    root_first = _host->_ch_data[order][i].first & LogicSnapshot::LSB;
  }

  return edge_hit;
}


// ----------------------------------------------------------------------------
// lbp_nxt_edge / block_nxt_edge / lbp_pre_edge / block_pre_edge
// ----------------------------------------------------------------------------

bool LogicSnapshotEdgeScan::lbp_nxt_edge(uint64_t &index, uint64_t root_index,
                                 uint64_t lbp_tog, uint8_t lbp_tog_pos,
                                 bool aft_tog, uint8_t aft_pos,
                                 bool last_sample, int sig_index) {
  // Guard: lbp_tog may be 0 when called with stale/empty data during
  // file loading. Original assert crashes via MSVC dialog.
  if (lbp_tog == 0) {
    pxv_warn("LogicSnapshot::lbp_nxt_edge: lbp_tog==0, skipping");
    return false;
  }

  // check last_sample with current index
  bool sample = _host->get_sample_self(index, sig_index);
  if (sample ^ last_sample) {
    return true;
  }

  // find edge between lbp
  bool edge_hit = false;
  uint64_t aft_lbp_start = (root_index << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower)) +
                           (aft_pos << LogicSnapshot::LeafBlockPower);

  while (lbp_tog_pos <= aft_pos) {
    uint64_t lbp_tog_index = (root_index << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower)) +
                             (lbp_tog_pos << LogicSnapshot::LeafBlockPower);
    if (lbp_tog_index > aft_lbp_start) {
      edge_hit = false;
      break;
    } else if (lbp_tog_index > index) {
      index = lbp_tog_index;
      edge_hit = true;
      break;
    }

    lbp_tog_pos++;
    lbp_tog &= (~0ULL << lbp_tog_pos);
    if ((lbp_tog_pos < LogicSnapshot::Scale) && (lbp_tog != 0)) {
      lbp_tog_pos = _host->bsf_folded(lbp_tog);
    } else {
      break;
    }
  }

  uint64_t lbp_edge_index =
      aft_tog ? aft_lbp_start : aft_lbp_start + (1ULL << LogicSnapshot::LeafBlockPower) - 1;
  if (!edge_hit && lbp_edge_index > index) {
    index = lbp_edge_index;
  }

  return edge_hit;
}

bool LogicSnapshotEdgeScan::block_nxt_edge(uint64_t *lbp, uint64_t &index,
                                   uint64_t block_end, bool last_sample,
                                   unsigned int min_level) {
  unsigned int level = min_level;
  bool fast_forward = true;
  const uint64_t last = last_sample ? ~0ULL : 0ULL;

  //----- Search Next Edge Within Current LeafBlock -----//
  if (level == 0) {
    // Search individual samples up to the beginning of
    // the next first level mip map block
    const uint64_t offset = (index & ~(~0ULL << LogicSnapshot::LeafBlockPower)) >> LogicSnapshot::ScalePower;
    const uint64_t mask = last_sample ? ~(~0ULL << (index & LogicSnapshot::LevelMask[0]))
                                      : ~0ULL << (index & LogicSnapshot::LevelMask[0]);
    uint64_t sample =
        last_sample ? *(lbp + offset) | mask : *(lbp + offset) & mask;
    if (sample ^ last) {
      index =
          (index & ~LogicSnapshot::LevelMask[0]) + _host->bsf_folded(last_sample ? ~sample : sample);
      fast_forward = false;
    } else {
      index = ((index >> LogicSnapshot::ScalePower) + 1) << LogicSnapshot::ScalePower;
    }
  } else {
    index = ((index >> level * LogicSnapshot::ScalePower) + 1) << level * LogicSnapshot::ScalePower;
  }

  if (fast_forward) {

    // Fast forward: This involves zooming out to higher
    // levels of the mip map searching for changes, then
    // zooming in on them to find the point where the edge
    // begins.

    // Zoom out at the beginnings of mip-map
    // blocks until we encounter a change
    while (index <= block_end) {
      // continue only within current block
      if (level == 0)
        level++;
      const int level_scale_power = (level + 1) * LogicSnapshot::ScalePower;
      const uint64_t offset =
          (index & ~(~0ULL << LogicSnapshot::LeafBlockPower)) >> level_scale_power;
      const uint64_t mask =
          ~0ULL << ((index & LogicSnapshot::LevelMask[level]) >> (level * LogicSnapshot::ScalePower));
      uint64_t sample = *(lbp + LogicSnapshot::LevelOffset[level] + offset) & mask;

      // Check if there was a change in this block
      if (sample) {
        index = (index & (~0ULL << (level + 1) * LogicSnapshot::ScalePower)) +
                (_host->bsf_folded(sample) << level * LogicSnapshot::ScalePower);
        break;
      } else {
        index = ((index >> (level + 1) * LogicSnapshot::ScalePower) + 1)
                << (level + 1) * LogicSnapshot::ScalePower;
        ++level;
      }
    }

    // Zoom in until we encounter a change,
    // and repeat until we reach min_level
    while ((index <= block_end) && (level > min_level)) {
      // continue only within current block
      level--;
      const int level_scale_power = (level + 1) * LogicSnapshot::ScalePower;
      const uint64_t offset =
          (index & ~(~0ULL << LogicSnapshot::LeafBlockPower)) >> level_scale_power;
      const uint64_t mask =
          (level == 0 && last_sample)
              ? ~(~0ULL << ((index & LogicSnapshot::LevelMask[level]) >> (level * LogicSnapshot::ScalePower)))
              : ~0ULL << ((index & LogicSnapshot::LevelMask[level]) >> (level * LogicSnapshot::ScalePower));
      uint64_t sample = (level == 0 && last_sample)
                            ? *(lbp + LogicSnapshot::LevelOffset[level] + offset) | mask
                            : *(lbp + LogicSnapshot::LevelOffset[level] + offset) & mask;

      // Update the low level position of the change in this block
      if (level == 0 ? sample ^ last : sample) {
        index = (index & (~0ULL << (level + 1) * LogicSnapshot::ScalePower)) +
                (_host->bsf_folded(level == 0 ? sample ^ last : sample)
                 << level * LogicSnapshot::ScalePower);
        if (level == min_level)
          break;
      }
    }
  }

  return (index <= block_end);
}

bool LogicSnapshotEdgeScan::lbp_pre_edge(uint64_t &index, uint64_t root_index,
                                 uint64_t lbp_tog, uint8_t &lbp_tog_pos,
                                 bool pre_tog, uint8_t pre_pos,
                                 bool last_sample, int sig_index) {
  // Guard: lbp_tog may be 0 when called with stale/empty data during
  // file loading. Original assert crashes via MSVC dialog.
  if (lbp_tog == 0) {
    pxv_warn("LogicSnapshot::lbp_pre_edge: lbp_tog==0, skipping");
    return false;
  }

  // check last_sample with current index
  bool sample = _host->get_sample_self(index, sig_index);
  if (sample ^ last_sample) {
    index++;
    return true;
  }

  // find edge between lbp
  bool edge_hit = false;
  uint64_t pre_lbp_end = (root_index << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower)) +
                         (pre_pos << LogicSnapshot::LeafBlockPower) +
                         (1ULL << LogicSnapshot::LeafBlockPower) - 1;

  do {
    uint64_t lbp_tog_index = (root_index << (LogicSnapshot::LeafBlockPower + LogicSnapshot::RootScalePower)) +
                             (lbp_tog_pos << LogicSnapshot::LeafBlockPower) +
                             (1ULL << LogicSnapshot::LeafBlockPower) - 1;
    if (lbp_tog_index < pre_lbp_end) {
      edge_hit = false;
      break;
    } else if (lbp_tog_index < index) {
      index = lbp_tog_index + 1;
      edge_hit = true;
      break;
    }

    if (lbp_tog_pos > 0) {
      lbp_tog_pos--;
      lbp_tog &= (~0ULL >> (LogicSnapshot::Scale - lbp_tog_pos - 1));
      lbp_tog_pos = (lbp_tog != 0) ? _host->bsr64(lbp_tog) : 0;
    } else {
      lbp_tog = 0;
    }
  } while (lbp_tog != 0 && lbp_tog_pos >= pre_pos);

  uint64_t lbp_edge_index =
      pre_tog ? pre_lbp_end : pre_lbp_end + 1 - (1ULL << LogicSnapshot::LeafBlockPower);
  if (!edge_hit && lbp_edge_index < index) {
    index = lbp_edge_index;
  }

  return edge_hit;
}

bool LogicSnapshotEdgeScan::block_pre_edge(uint64_t *lbp, uint64_t &index,
                                   bool last_sample, unsigned int min_level,
                                   int sig_index) {
  // Guard: min_level > 0 is not expected, but can happen with stale state.
  if (min_level != 0) {
    pxv_warn("LogicSnapshot::block_pre_edge: min_level=%u != 0, skipping", min_level);
    return false;
  }

  unsigned int level = min_level;
  bool fast_forward = true;
  const uint64_t last = last_sample ? ~0ULL : 0ULL;
  uint64_t block_start = index & ~LogicSnapshot::LeafMask;

  if (!lbp) {
    pxv_warn("%s", "LogicSnapshot::block_pre_edge: lbp is nullptr");
    return false;
  }
  assert(lbp);

  //----- Search Next Edge Within Current LeafBlock -----//
  if (level == 0) {
    // Search individual samples down to the beginning of
    // the previous first level mip map block
    const uint64_t offset = (index & ~(~0ULL << LogicSnapshot::LeafBlockPower)) >> LogicSnapshot::ScalePower;
    const uint64_t mask = last_sample
                              ? ~(~0ULL >> (LogicSnapshot::Scale - (index & LogicSnapshot::LevelMask[0]) - 1))
                              : ~0ULL >> (LogicSnapshot::Scale - (index & LogicSnapshot::LevelMask[0]) - 1);
    uint64_t sample =
        last_sample ? *(lbp + offset) | mask : *(lbp + offset) & mask;
    if (sample ^ last) {
      index =
          (index & ~LogicSnapshot::LevelMask[0]) + _host->bsr64(last_sample ? ~sample : sample) + 1;
      return true;
    } else {
      index &= ~LogicSnapshot::LevelMask[0];
      if (index == 0)
        return false;
      else
        index--;

      // using get_sample_self() to avoid out of block case
      bool sample = _host->get_sample_self(index, sig_index);
      if (sample ^ last_sample) {
        index++;
        return true;
      } else if (index < block_start) {
        return false;
      }
    }
  }

  if (fast_forward) {

    // Fast forward: This involves zooming out to higher
    // levels of the mip map searching for changes, then
    // zooming in on them to find the point where the edge
    // begins.

    // Zoom out at the beginnings of mip-map
    // blocks until we encounter a change
    while (index > block_start) {
      // continue only within current block
      if (level == 0)
        level++;
      const int level_scale_power = (level + 1) * LogicSnapshot::ScalePower;
      const uint64_t offset =
          (index & ~(~0ULL << LogicSnapshot::LeafBlockPower)) >> level_scale_power;
      const uint64_t mask =
          ~0ULL >>
          (LogicSnapshot::Scale - ((index & LogicSnapshot::LevelMask[level]) >> (level * LogicSnapshot::ScalePower)) - 1);
      uint64_t sample = *(lbp + LogicSnapshot::LevelOffset[level] + offset) & mask;

      // Check if there was a change in this block
      if (sample) {
        index = (index & (~0ULL << (level + 1) * LogicSnapshot::ScalePower)) +
                (_host->bsr64(sample) << level * LogicSnapshot::ScalePower) +
                ~(~0ULL << level * LogicSnapshot::ScalePower);
        break;
      } else {
        index = (index >> (level + 1) * LogicSnapshot::ScalePower) << (level + 1) * LogicSnapshot::ScalePower;
        if (index == 0)
          return false;
        else
          index--;
      }
    }

    // Zoom in until we encounter a change,
    // and repeat until we reach min_level
    while ((index >= block_start) && (level > min_level)) {
      // continue only within current block
      level--;
      const int level_scale_power = (level + 1) * LogicSnapshot::ScalePower;
      const uint64_t offset =
          (index & ~(~0ULL << LogicSnapshot::LeafBlockPower)) >> level_scale_power;
      const uint64_t mask =
          (level == 0 && last_sample)
              ? ~(~0ULL >>
                  (LogicSnapshot::Scale -
                   ((index & LogicSnapshot::LevelMask[level]) >> (level * LogicSnapshot::ScalePower)) - 1))
              : ~0ULL >>
                    (LogicSnapshot::Scale -
                     ((index & LogicSnapshot::LevelMask[level]) >> (level * LogicSnapshot::ScalePower)) - 1);
      uint64_t sample = (level == 0 && last_sample)
                            ? *(lbp + LogicSnapshot::LevelOffset[level] + offset) | mask
                            : *(lbp + LogicSnapshot::LevelOffset[level] + offset) & mask;

      // Update the low level position of the change in this block
      if (level == 0 ? sample ^ last : sample) {
        index =
            (index & (~0ULL << (level + 1) * LogicSnapshot::ScalePower)) +
            (_host->bsr64(level == 0 ? sample ^ last : sample) << level * LogicSnapshot::ScalePower) +
            ~(~0ULL << level * LogicSnapshot::ScalePower);
        if (level == min_level) {
          index++;
          break;
        }
      } else {
        index = (index & (~0ULL << (level + 1) * LogicSnapshot::ScalePower));
      }
    }
  }

  return (index >= block_start) && (index != 0);
}

}  // namespace data
}  // namespace pv
