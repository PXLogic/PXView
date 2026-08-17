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

#include <algorithm>
#include <cassert>
#include <atomic>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/utility/array.h"
#include "pv/data/snapshot/leaf_block_pool.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/snapshot/logicsnapshot_diskcache_writer.h"
#include "pv/data/snapshot/logicsnapshot_glitch_filter.h"
#include "pv/data/snapshot/logicsnapshot_pattern_search.h"
#include "pv/data/snapshot/logicsnapshot_edge_scan.h"
#include <map>
#include <string>

using namespace std;

namespace pv {
namespace data {

namespace {
/* [PathDiag] 窗口累计器 — 每 50 个 payload 打印一次 avg/max 汇总。
 *
 * raw 版三段时间口径 (spec 阶段3): 锁等待 (lockwait, 唯一未被覆盖的盲区) /
 * 写块 (write, 锁外 memcpy 写 mmap 页) / metadata (meta, 锁内
 * allocate_block + calc_mipmap + 计数, 由 total−lockwait−write 导出). raw
 * 无显式 mmap flush 段 (OS 页缓存管理). 旧版"每 50 个打印瞬时值"会漏掉窗口
 * 内极慢 payload, 窗口 max 专抓它们. */
struct PathDiagWindow {
    uint64_t count = 0;
    double sum_ms = 0, max_ms = 0;
    double lockwait_sum = 0, lockwait_max = 0;
    double write_sum = 0, write_max = 0;
    double meta_sum = 0, meta_max = 0;
    uint64_t max_payload = 0;

    void add(uint64_t payload_no, uint64_t payload_bytes,
             double total_ms, double lockwait_ms, double write_ms) {
        double meta_ms = total_ms - lockwait_ms - write_ms;
        if (meta_ms < 0) meta_ms = 0;  // 计时噪声兜底
        count++;
        sum_ms += total_ms;
        lockwait_sum += lockwait_ms;
        write_sum += write_ms;
        meta_sum += meta_ms;
        if (lockwait_ms > lockwait_max) lockwait_max = lockwait_ms;
        if (write_ms > write_max) write_max = write_ms;
        if (meta_ms > meta_max) meta_max = meta_ms;
        if (total_ms > max_ms) { max_ms = total_ms; max_payload = payload_no; }
        if (count % 50 == 0) {
            const double avg = sum_ms / 50.0;
            const double mbps = avg > 0
                ? (double)payload_bytes / 1e6 / (avg / 1000.0) : 0.0;
            pxv_info("[PathDiag] win#%llu (50 payloads): avg=%.2fms (%.0f MB/s) "
                     "max=%.1fms @#%llu | lockwait avg=%.2f max=%.2f | "
                     "write avg=%.2f max=%.2f | meta avg=%.2f max=%.2f",
                     (unsigned long long)(count / 50), avg, mbps, max_ms,
                     (unsigned long long)max_payload,
                     lockwait_sum / 50.0, lockwait_max,
                     write_sum / 50.0, write_max,
                     meta_sum / 50.0, meta_max);
            sum_ms = 0; max_ms = 0;
            lockwait_sum = 0; lockwait_max = 0;
            write_sum = 0; write_max = 0;
            meta_sum = 0; meta_max = 0;
        }
    }
};

} // anonymous namespace

const uint64_t LogicSnapshot::LevelMask[LogicSnapshot::ScaleLevel] = {
    ~(~0ULL << ScalePower) << 0 * ScalePower,
    ~(~0ULL << ScalePower) << 1 * ScalePower,
    ~(~0ULL << ScalePower) << 2 * ScalePower,
    ~(~0ULL << ScalePower) << 3 * ScalePower,
};
const uint64_t LogicSnapshot::LevelOffset[LogicSnapshot::ScaleLevel] = {
    0,
    (uint64_t)pow(Scale, 3),
    (uint64_t)pow(Scale, 3) + (uint64_t)pow(Scale, 2),
    (uint64_t)pow(Scale, 3) + (uint64_t)pow(Scale, 2) + (uint64_t)pow(Scale, 1),
};

LogicSnapshot::LogicSnapshot() : Snapshot(1, 0, 0) {
  _channel_num = 0;
  _total_sample_count = 0;
  _is_loop = false;
  _loop_offset = 0;
  _able_free = true;
  _mmap_alloc = nullptr;
  _max_blocks_per_channel = 0;
  _disk_cache_writer = std::make_unique<LogicSnapshotDiskCacheWriter>(this);
  _glitch_filter = std::make_unique<LogicSnapshotGlitchFilter>(this);
  _pattern_search = std::make_unique<LogicSnapshotPatternSearch>(this);
  _edge_scan = std::make_unique<LogicSnapshotEdgeScan>(this);
}
LogicSnapshot::~LogicSnapshot() {
  // PulseView pattern: derived destructor explicitly frees its own data.
  //
  // Previously this destructor was empty, relying on unique_ptr member
  // destructors. But the base Snapshot::~Snapshot() calls free_data()
  // which — due to C++ [class.dtor]/12 — resolves to Snapshot::free_data()
  // (base version), NOT LogicSnapshot::free_data(). This meant non-mmap
  // LeafBlockPool blocks leaked and mmap slot bitmaps were never cleared.
  //
  // Now: explicitly drain the async writer thread and call free_data()
  // in the derived destructor body (where the vtable is still
  // LogicSnapshot's). drain_and_join() is safe even if the writer was
  // already stopped (it checks _async_running before joining).
  _disk_cache_writer->drain_and_join();
  free_data();
}
void LogicSnapshot::free_data() {
  // P1-6 fix: Skip memory optimization if there are active iterators
  // (e.g. the decode thread calling get_samples). This prevents
  // use-after-free when free_data is called from clear/copy_from while
  // the decode thread holds a raw pointer into the data.
  if (has_active_iterators()) {
    pxv_info("LogicSnapshot::free_data: deferred — %d active iterators",
             _iterator_count.load());
    return;
  }

  // Clear the mmap slot bitmap via the writer (it owns _mmap_slot_written).
  _disk_cache_writer->clear_all_mmap_slots();

  Snapshot::free_data();

  // Return non-mmap leaf blocks to LeafBlockPool before discarding _ch_data.
  // When mmap allocation succeeds, all lbp pointers are mmap-backed and are
  // freed by _mmap_alloc.reset() below. But when mmap configure fails and the
  // code falls back to LeafBlockPool::acquire(), those blocks are NOT mmap-backed
  // and must be explicitly returned — otherwise they leak (the swap below would
  // discard the only references to them).
  // NOTE: the is_mmap_address check must happen BEFORE _mmap_alloc.reset(),
  // because after reset() the allocator is null and the mmap region is unmapped.
  for (auto &iter : _ch_data) {
    for (auto &rn : iter) {
      for (int k = 0; k < (int)Scale; k++) {
        if (rn.lbp[k] != nullptr) {
          // If the pointer is NOT in the mmap region, it came from
          // LeafBlockPool — return it.
          if (!_mmap_alloc || !_mmap_alloc->is_mmap_address(rn.lbp[k])) {
            LeafBlockPool::instance().release(rn.lbp[k]);
          }
          rn.lbp[k] = nullptr;
        }
      }
    }
    std::vector<struct RootNode> void_vector;
    iter.swap(void_vector);
  }
  _ch_data.clear();
  _sample_count = 0;

  // Now safe to release the mmap allocator — all non-mmap blocks have been
  // returned to LeafBlockPool, and mmap-backed blocks are unmapped by reset().
  _mmap_alloc.reset();

  for (void *p : _free_block_list) {
    LeafBlockPool::instance().release(p);
  }
  _free_block_list.clear();
}

void LogicSnapshot::init() {
  std::lock_guard<std::recursive_mutex> lock(_mutex);
  init_all();
}

void LogicSnapshot::init_all() {
  _sample_count = 0;
  _ring_sample_count = 0;
  _ring_published.store(0, std::memory_order_release);
  _byte_fraction = 0;
  _ch_fraction = 0;
  _dest_ptr = nullptr;
  _memory_failed = false;
  _last_ended = true;
  _loop_offset = 0;
  _able_free = true;
}

void LogicSnapshot::clear() {
  // CRITICAL (Task 1): stop the async writer and drain its queue BEFORE
  // free_data() resets the mmap allocator. Encapsulated in drain_and_join().
  _disk_cache_writer->drain_and_join();

  std::lock_guard<std::recursive_mutex> lock(_mutex);
  free_data();
  init_all();
}

void LogicSnapshot::set_disk_cache_config(const DiskCacheConfig &config) {
  _disk_cache_writer->set_disk_cache_config(config);
}

bool LogicSnapshot::is_disk_cache_active() {
  return _disk_cache_writer->is_disk_cache_active();
}

double LogicSnapshot::get_disk_write_speed_mbps() {
  return _disk_cache_writer->get_disk_write_speed_mbps();
}

size_t LogicSnapshot::get_disk_write_queue_depth() {
  return _disk_cache_writer->get_disk_write_queue_depth();
}

uint64_t LogicSnapshot::get_disk_total_blocks_written() {
  return _disk_cache_writer->get_disk_total_blocks_written();
}

uint64_t LogicSnapshot::get_page_fault_count() {
  uint64_t current_pf = 0;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc;
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    current_pf = pmc.PageFaultCount;
  }
#elif defined(__linux__) || defined(__APPLE__)
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
    current_pf = ru.ru_minflt + ru.ru_majflt;
  }
#endif

  auto now = std::chrono::steady_clock::now().time_since_epoch();
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  int64_t last_time = _last_pf_time.load();
  
  if (now_ms - last_time >= 1000) {
      uint64_t last_pf = _last_pf_count.load();
      _pf_per_sec = current_pf >= last_pf ? (current_pf - last_pf) : 0;
      _last_pf_count = current_pf;
      _last_pf_time = now_ms;
  }
  
  return _pf_per_sec.load();
}

uint64_t LogicSnapshot::get_working_set_bytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc;
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return pmc.WorkingSetSize;
  }
#elif defined(__linux__) || defined(__APPLE__)
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __linux__
    return ru.ru_maxrss * 1024;
#else
    return ru.ru_maxrss;
#endif
  }
#endif
  return 0;
}

uint64_t LogicSnapshot::get_async_queue_bytes() {
  return _disk_cache_writer->get_async_queue_bytes();
}

uint64_t LogicSnapshot::get_mmap_total_bytes() {
  if (_mmap_alloc)
    return _mmap_alloc->get_total_bytes();
  return 0;
}

void LogicSnapshot::ensure_all_blocks_hot() {
  _disk_cache_writer->ensure_all_blocks_hot();
}

void LogicSnapshot::first_payload(const sr_datafeed_logic &logic,
                                  uint64_t total_sample_count, GSList *channels,
                                  bool able_free) {
  auto _fp_t0 = std::chrono::steady_clock::now();
  bool channel_changed = false;
  uint16_t channel_num = 0;
  _able_free = able_free;
  _lst_free_block_index = 0;

  for (void *p : _free_block_list) {
    LeafBlockPool::instance().release(p);
  }
  _free_block_list.clear();

  for (const GSList *l = channels; l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;
    if (probe->type == SR_CHANNEL_LOGIC && probe->enabled) {
      channel_num++;
      if (!channel_changed) {
        channel_changed = !has_data(probe->index);
      }
    }
  }

  std::unique_lock<std::recursive_mutex> lock(_mutex);

  // 问题3防御：total_sample_count 来自 device.get_sample_limit()。对文件设备
  // （pxl 回放），若 header "total samples" 缺失/损坏，该值可能回退为巨大的
  // 垃圾值（如 UINT64_MAX 溢出解析），导致下方 root_vector 分配 bad_alloc 而
  // 在捕获线程抛出未捕获异常崩溃。这里做上限保护：任何超过 2^48 个样本
  // （约 281 万亿）的声明都视为垃圾值，回退到 1M（与 default_sample_limit 一致）。
  // 正常采集（含 250MHz×数小时）远低于此上限，不受影响。
  // 注意：本文件被 qtest 直接编译（不链接 pxview-config），不能依赖
  // AppConfig::Instance()，故使用本地常量兜底。
  constexpr uint64_t kSanityMaxSamples = (1ULL << 48);
  constexpr uint64_t kFallbackSampleLimit = 1000000ULL;
  if (total_sample_count > kSanityMaxSamples) {
    pxv_err("first_payload: suspicious total_sample_count=%llu, clamping "
            "to fallback sample limit",
            (unsigned long long)total_sample_count);
    total_sample_count = kFallbackSampleLimit;
  }

  if (total_sample_count != _total_sample_count ||
      channel_num != _channel_num || channel_changed || _is_loop) {

    // CRITICAL (Task 1): stop the async writer and drain its queue BEFORE
    // free_data() resets the mmap allocator. Otherwise the async worker could
    // still be calling allocate_block()/_mmap_alloc->get_block_data() against
    // an allocator we are about to destroy, causing a use-after-free / crash.
    // Mirrors the stop sequence in clear() — now encapsulated in drain_and_join().
    auto _drain_t0 = std::chrono::steady_clock::now();
    _disk_cache_writer->drain_and_join();
    auto _drain_t1 = std::chrono::steady_clock::now();
    pxv_info("first_payload TIMING: drain_and_join=%lldms",
      (long long)std::chrono::duration_cast<std::chrono::milliseconds>(_drain_t1 - _drain_t0).count());

    auto _free_t0 = std::chrono::steady_clock::now();
    free_data();
    auto _free_t1 = std::chrono::steady_clock::now();
    pxv_info("first_payload TIMING: free_data=%lldms",
      (long long)std::chrono::duration_cast<std::chrono::milliseconds>(_free_t1 - _free_t0).count());

    _ch_index.clear();

    _total_sample_count = total_sample_count;
    _channel_num = channel_num;
    uint64_t rootnode_size =
        (_total_sample_count + RootNodeSamples - 1) / RootNodeSamples;

    if (_is_loop) {
      rootnode_size += 2;
    }

    for (const GSList *l = channels; l; l = l->next) {
      sr_channel *const probe = (sr_channel *)l->data;

      if (probe->type == SR_CHANNEL_LOGIC && probe->enabled) {
        std::vector<struct RootNode> root_vector;

        for (uint64_t j = 0; j < rootnode_size; j++) {
          struct RootNode rn;
          rn.tog = 0;
          rn.first = 0;
          rn.last = 0;
          memset(rn.lbp, 0, sizeof(rn.lbp));
          root_vector.push_back(rn);
        }

        _ch_data.push_back(root_vector);
        _ch_index.push_back(probe->index);
      }
    }

    if (_ch_index.size() == 0) {
      pxv_err("LogicSnapshot: all channels disabled, aborting");
      return;
    }
  } else {
    // FREE leaf blocks instead of zeroing in-place. The old in-place memset
    // destroyed data that might still be read by a concurrent decoder (which
    // shares the same LogicSnapshot object as capture_data when config is
    // unchanged). By freeing (set lbp=nullptr, return to pool), allocate_block
    // will hand out fresh zeroed blocks for the new capture — and the old
    // blocks remain valid in the pool until recycled, so any concurrent
    // reader holding a reference to them via free_decode_lpb is safe.
    //
    // CRITICAL FIX (SIGSEGV in repeat mode): Do NOT call push_to_free_list
    // here. push_to_free_list calls decommit_block on mmap-backed blocks,
    // which uses VirtualFree(MEM_DECOMMIT) to return physical pages to the
    // OS. In repeat mode, the decoder thread from the PREVIOUS capture may
    // still be running (it was started by CopyToDocDone and hasn't been
    // stopped yet — clear_all_decode_task2 only runs on the NEXT
    // RevEndPacket). The decoder holds raw pointers (di->inbuf) into these
    // leaf blocks via get_samples(). If we decommit the pages here, the
    // decoder thread hits decommitted virtual memory → SIGSEGV in
    // term_matches (instance.c:1213).
    //
    // Instead, just reset the metadata (tog/first/last) and leave the lbp
    // pointers intact. The new capture's allocate_block() will find the
    // existing blocks and return them immediately (no zeroing, no
    // reallocation). The new capture's append_*_payload will overwrite the
    // old data in-place. The decoder may read a mix of old and new data,
    // but: (1) it won't crash, and (2) its results will be discarded when
    // the next RevEndPacket calls clear_all_decode_task2.
    for (auto &iter : _ch_data) {
      for (auto &iter_rn : iter) {
        iter_rn.tog = 0;
        iter_rn.first = 0;
        iter_rn.last = 0;
        // Leaf block pointers (lbp[]) are intentionally NOT freed and NOT
        // set to nullptr. They remain committed for safe concurrent decoder
        // access. allocate_block() will reuse them in-place.
      }
    }
  }

  assert(_channel_num < CHANNEL_MAX_COUNT);

  _sample_count = 0;
  _ring_sample_count = 0;
  // C3: reset published count so a concurrent finite reader does not observe
  // stale samples from a prior capture during the reuse (config-unchanged)
  // window before the first append of this capture publishes fresh counts.
  _ring_published.store(0, std::memory_order_release);
  // CRITICAL FIX: 重置 _loop_offset。first_payload 在配置未变（reuse 同样
  // total_sample_count/channel_num）的复用路径下不会调用 free_data()/init_all()，
  // 导致上一次 capture 在 append_payload_impl 中错误累积的 _loop_offset 残留，
  // 下次 capture 的 _ring_sample_count += _loop_offset 会从错误基址开始。
  _loop_offset = 0;

  // CRITICAL FIX (SIGSEGV): Both branches above freed leaf blocks via
  // push_to_free_list, which calls decommit_block on mmap-backed blocks,
  // returning physical pages to the OS. However _dest_ptr may still point
  // into a now-decommitted virtual address range. If the previous capture
  // ended mid-chunk (_ch_fraction != 0 || _byte_fraction != 0), the next
  // append_cross_payload's bit-align phase would dereference _dest_ptr and
  // hit decommitted pages -> SIGSEGV. Reset all bit-align state here.
  _dest_ptr = nullptr;
  _ch_fraction = 0;
  _byte_fraction = 0;
  _last_ended = true;

  for (unsigned int i = 0; i < _channel_num; i++) {
    _last_sample[i] = 0;
    _last_calc_count[i] = 0;
    _cur_ref_block_indexs[i].root_index = 0;
    _cur_ref_block_indexs[i].lbp_index = 0;
  }

  pxv_info("LogicSnapshot::first_payload: disk_cache_config.enabled=%d, "
           "ch_data.size()=%zu",
           _disk_cache_writer->disk_cache_config().enabled, _ch_data.size());

  if (_channel_num > 0) {
    // CRITICAL FIX (SIGSEGV in repeat mode): Only create a new MmapAllocator
    // if there isn't one already. In repeat mode (else branch above, config
    // unchanged), _mmap_alloc still points to the old allocator. Replacing it
    // with a new one would destroy the old shared_ptr → MmapAllocator
    // destructor calls UnmapViewOfFile → the entire old mmap region is
    // unmapped. The decoder thread from the previous capture (still running,
    // started by CopyToDocDone) holds raw pointers (di->inbuf) into the old
    // mmap → SIGSEGV in term_matches (instance.c:1238).
    //
    // In the if branch (config changed), free_data() already reset
    // _mmap_alloc to nullptr, so a new allocator is created. On the very
    // first call, _mmap_alloc is also nullptr (initialized in constructor).
    if (!_mmap_alloc) {
      // Create and configure MmapAllocator
      _mmap_alloc = std::make_shared<MmapAllocator>();

      // Calculate total required memory based on total_sample_count + padding
      // For loop mode, _total_sample_count is the size of the ring buffer.
      _max_blocks_per_channel = (_total_sample_count / LeafBlockSamples) + 16;
      uint64_t total_bytes = _max_blocks_per_channel * LeafBlockSpace * _channel_num;

      bool use_disk = _disk_cache_writer->disk_cache_config().enabled;
      // P4 整文件预分配上限 (spec 阶段3): 磁盘模式按 DiskCacheConfig 的
      // total_cache_depth_gb 封顶, 采集深度由盘容量决定而非内存. 每通道均分
      // (layout 已按 channel * max_blocks_per_channel 分片). 超过盘容量时截断
      // _max_blocks_per_channel 与 total_bytes, 采集超出时由 allocate_block /
      // get_block_data 越界报错并回退 LeafBlockPool.
      if (use_disk) {
        const uint64_t disk_cap_bytes =
            _disk_cache_writer->disk_cache_config().total_cache_depth_gb *
            (1024ULL * 1024 * 1024);
        if (disk_cap_bytes > 0 && total_bytes > disk_cap_bytes) {
          pxv_warn("LogicSnapshot::first_payload: capture needs %llu bytes > "
                   "disk cap %llu GB, clamping (depth becomes disk-limited)",
                   (unsigned long long)total_bytes,
                   (unsigned long long)_disk_cache_writer->disk_cache_config().total_cache_depth_gb);
          // 均分到每通道后折算成每通道块数 (整块对齐)
          _max_blocks_per_channel =
              disk_cap_bytes / (LeafBlockSpace * _channel_num);
          total_bytes = _max_blocks_per_channel * LeafBlockSpace * _channel_num;
        }
      }

      QString disk_dir = QString::fromStdString(_disk_cache_writer->disk_cache_config().cache_path);
      auto _mmap_t0 = std::chrono::steady_clock::now();
      bool mmap_ok = _mmap_alloc->configure(use_disk, disk_dir, total_bytes,
                                            LeafBlockSpace, _max_blocks_per_channel, _channel_num);
      auto _mmap_t1 = std::chrono::steady_clock::now();
      pxv_info("first_payload TIMING: MmapAllocator::configure=%lldms (total_bytes=%llu, ok=%d)",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(_mmap_t1 - _mmap_t0).count(),
        (unsigned long long)total_bytes, (int)mmap_ok);
      if (!mmap_ok) {
          pxv_err("LogicSnapshot::first_payload: MmapAllocator configure failed! "
                 "Falling back to LeafBlockPool in-memory allocation.");
          // Set _memory_failed so the user gets a dialog warning that mmap
          // allocation failed and the system is running in degraded mode
          // (LeafBlockPool fallback). The datafeedparser checks memory_failed()
          // after append_payload and triggers session_error() -> dialog.
          _memory_failed = true;
          // Drop the failed allocator so allocate_block() takes the
          // LeafBlockPool::instance().acquire() fallback path instead of
          // dereferencing an unconfigured (nullptr _base_ptr) allocator.
          _disk_cache_writer->clear_all_mmap_slots();
          _mmap_alloc.reset();
      } else {
          // 设置 loop mode（loop mode 禁用 trailing decommit，保留所有数据在 RAM）
          _mmap_alloc->set_loop_mode(_is_loop);
          // 等待 prefault 线程完成初始 4 blocks 超前（4 * 16ch * 2MB = 128MB）
          auto _pf_t0 = std::chrono::steady_clock::now();
          _mmap_alloc->wait_prefault_initial_blocks(4);
          auto _pf_t1 = std::chrono::steady_clock::now();
          pxv_info("first_payload TIMING: wait_prefault_initial_blocks(4)=%lldms",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(_pf_t1 - _pf_t0).count());
      }
    } else {
// Reusing existing MmapAllocator (repeat mode, config unchanged).
// The mmap region stays mapped — decoder threads from the previous
// capture can safely read from it while the new capture overwrites
// data in-place. No need to re-configure: same total_sample_count,
// channel_num, and disk_cache_config.
//
// IMPORTANT: Do NOT restart the prefault thread here. All pages are
// already committed from the previous capture. The prefault worker
// writes zeros to every page (to force page-table entries), which
// would overwrite the previous capture's data. Since the UI renders
// from the same mmap (view_data == capture_data in non-stream repeat
// mode), zeroing the blocks causes a blank screen on stop.
_mmap_alloc->stop_prefault();
pxv_info("first_payload: reusing existing MmapAllocator (repeat mode, prefault not restarted)");
    }
  }

  if (_mmap_alloc) {
    _disk_cache_writer->setup_mmap_slots(_channel_num * _max_blocks_per_channel);
  }

  _disk_cache_writer->start();

  lock.unlock();
  append_payload(logic);
  _last_ended = false;

  auto _fp_t1 = std::chrono::steady_clock::now();
  pxv_info("first_payload TIMING: total=%lldms (samplerate will be logged by caller)",
    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(_fp_t1 - _fp_t0).count());
}

void LogicSnapshot::append_payload(const sr_datafeed_logic &logic) {
  // Dispatch by format:
  //   LA_CROSS_DATA: raw channel-block (PXLogic/DSLogic fork) — forwarded to
  //     the async worker which calls append_cross_payload (v1.49 bit-copy,
  //     no deinterleave). This avoids the ~100ms/4MB deinterleave bottleneck
  //     in the driver's USB receive path.
  //   LA_SPLIT_DATA (default): sample-interleaved (upstream sigrok) — forwarded
  //     to the async worker which calls append_payload_impl.
  // The async worker preserves format and does the actual chunk-tree write
  // under _mutex, with 256MB backpressure between feed thread and disk write.
  _disk_cache_writer->enqueue((const uint8_t *)logic.data, logic.length,
                              logic.format);
}

bool LogicSnapshot::is_mmap_slot_fresh(uint16_t channel, uint64_t global_block_seq) const {
    return _disk_cache_writer->is_mmap_slot_fresh(channel, global_block_seq);
}

void LogicSnapshot::mark_mmap_slot_written(uint16_t channel, uint64_t global_block_seq) {
    _disk_cache_writer->mark_mmap_slot_written(channel, global_block_seq);
}

void LogicSnapshot::clear_mmap_slot_written(uint16_t channel, uint64_t global_block_seq) {
    _disk_cache_writer->clear_mmap_slot_written(channel, global_block_seq);
}

void LogicSnapshot::clear_mmap_slot_by_abs(uint64_t abs_slot) {
    _disk_cache_writer->clear_mmap_slot_by_abs(abs_slot);
}

void* LogicSnapshot::allocate_block(uint16_t channel, uint64_t index0, uint64_t index1) {
    void* lbp = _ch_data[channel][index0].lbp[index1];
    if (lbp != nullptr) return lbp;

    bool from_mmap = false;
    uint64_t global_block_seq = 0;
    if (_mmap_alloc) {
        global_block_seq = index0 * RootScale + index1;
        lbp = _mmap_alloc->get_block_data(channel, global_block_seq, _max_blocks_per_channel, LeafBlockSpace);
        from_mmap = (lbp != nullptr);
        if (from_mmap) {
            // 通知 prefault 线程 writer 进度（block_seq，所有 channel 同步写入同一 block_seq）
            _mmap_alloc->notify_writer_block_seq(global_block_seq);
        }
    }
    if (lbp == nullptr) {
        lbp = LeafBlockPool::instance().acquire(LeafBlockSpace);
        if (lbp == nullptr) {
            pxv_err("LogicSnapshot: Malloc memory failed!");
            _memory_failed = true;
            return nullptr;
        }
    }
    _ch_data[channel][index0].lbp[index1] = lbp;

    if (from_mmap) {
        // mmap 首次分配：OS 懒加载零填充，跳过 memset；复用槽位（wrap）需清零残留。
        if (!is_mmap_slot_fresh(channel, global_block_seq)) {
            memset(lbp, 0, LeafBlockSpace);
        }
        mark_mmap_slot_written(channel, global_block_seq);
    } else {
        // LeafBlockPool 回收块：可能含脏数据，必须清零。
        memset(lbp, 0, LeafBlockSpace);
    }
    return lbp;
}

void LogicSnapshot::append_payload_impl(const sr_datafeed_logic &logic) {
  // Sample-interleaved input (upstream libsigrok 0.6 format).
  // logic.data: logic.length bytes; unitsize = (_channel_num+7)/8 bytes per
  // sample group. Sample s channel ch bit = src[s*unitsize + ch/8] bit (ch%8).
  // Chunk tree stores per-channel data, 8 samples packed per byte (LSB-first).
  // Process in 64-sample batches (Scale) to match mipmap granularity; residual
  // 1-7 samples at the tail are tracked via _byte_fraction for continuation.

  assert(logic.data);
  assert(_channel_num > 0);

  if (logic.length == 0) return;

  uint64_t unitsize = logic.unitsize;
  if (unitsize == 0) unitsize = (_channel_num + 7) / 8;
  uint64_t num_samples = logic.length / unitsize;

  if (num_samples == 0) return;

  // [PathDiag] 入口计时起点 (含 _mutex 获取等待 — 唯一未被写块/元数据覆盖的盲区)
  static PathDiagWindow pd_win;
  static uint64_t pd_seq = 0;
  const auto pd_entry = std::chrono::steady_clock::now();
  double pd_lockwait_ms = 0.0, pd_write_ms = 0.0;
  // RAII 汇总器: 函数所有 return 路径统一记账 (只统计真正处理的 payload)
  struct PdFinalize {
    PathDiagWindow &w; uint64_t &seq; uint64_t bytes;
    std::chrono::steady_clock::time_point t0;
    double &lw; double &wr;
    ~PdFinalize() {
      const double total = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0).count();
      w.add(++seq, bytes, total, lw, wr);
    }
  } pd_fin{pd_win, pd_seq, (uint64_t)logic.length, pd_entry, pd_lockwait_ms, pd_write_ms};

  // Segmented locking: metadata ops (allocate_block, _sample_count/
  // _ring_sample_count updates, calc_mipmap, loop housekeeping) hold _mutex;
  // mmap data writes (page-fault-prone) release it so UI's get_samples can
  // proceed. _dest_ptr/_ch_fraction/_byte_fraction are async-writer-private
  // and safe to touch without the lock during write loops.
  std::unique_lock<std::recursive_mutex> lock(_mutex);
  pd_lockwait_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - pd_entry).count();

  // Update _sample_count (cap at _total_sample_count)
  if (_sample_count + num_samples < _total_sample_count) {
    _sample_count += num_samples;
  } else {
    if (_sample_count == _total_sample_count && !_is_loop)
      return;
    // CRITICAL FIX: 截断 num_samples 到剩余样本数，避免处理超出 _total_sample_count
    // 的样本。否则 phase 1-5 会处理多余样本，使 absolute_position >
    // _total_sample_count，触发下方 _loop_offset 的错误赋值（非 loop 模式下
    // _loop_offset 应恒为 0）。错误累积后会在 capture_ended 中放大
    // _ring_sample_count，导致 index0/index1/offset 越界指向未分配的 leaf block。
    num_samples = _total_sample_count - _sample_count;
    _sample_count = _total_sample_count;
  }

  // Loop mode housekeeping (unchanged from channel-block version)
  if (_is_loop) {
    if (_loop_offset >= LeafBlockSamples * Scale) {
      move_first_node_to_last();
      _loop_offset -= LeafBlockSamples * Scale;
      _lst_free_block_index = 0;
      // Invalidate stale _dest_ptr: move_first_node_to_last decommits all
      // Scale leaf blocks in root node 0. append_payload_impl does not use
      // _dest_ptr directly (it uses ch_lbp[]), but reset for safety in case
      // a future format switch reuses the stale pointer.
      _dest_ptr = nullptr;
      _ch_fraction = 0;
      _byte_fraction = 0;
    } else {
      int free_count = _loop_offset / LeafBlockSamples;
      if (free_count > _lst_free_block_index) {
        free_head_blocks(free_count);
        _dest_ptr = nullptr;
        _ch_fraction = 0;
        _byte_fraction = 0;
      }
    }
  }

  _ring_sample_count += _loop_offset;

  // CRITICAL FIX: align_sample_count 必须对齐到 LeafBlockSamples 边界（block 起始），
  // offset 是 block 内偏移。旧代码 align_sample_count = _ring_sample_count（未对齐），
  // 导致末尾 absolute_position = align_sample_count + offset 重复累加初始偏移，
  // 每次 append_payload_impl 调用 _ring_sample_count 指数膨胀
  // （r_n = 2 * r_{n-1} + num_samples），25 个 packet 后从 100000 膨胀到 2.3 亿。
  uint64_t offset = _ring_sample_count % LeafBlockSamples;
  uint64_t align_sample_count = _ring_sample_count - offset;  // 对齐到 leaf block 起始
  uint64_t index0 = align_sample_count / LeafBlockSamples / RootScale;
  uint64_t index1 = (align_sample_count / LeafBlockSamples) % RootScale;
  // Derive _byte_fraction from the persisted ring position (robust against
  // copy_from / loop-mode state transitions).
  _byte_fraction = (uint8_t)(offset % 8);

  if (index0 >= _ch_data[0].size()) {
    pxv_err("append_payload_impl: index0 %llu out of range %zu",
            (unsigned long long)index0, _ch_data[0].size());
    _ring_sample_count = align_sample_count + offset - _loop_offset;
    _ch_fraction = 0;
    _dest_ptr = nullptr;
    return;
  }

  // Cache leaf block pointers per channel (re-allocated at leaf block boundary)
  void *ch_lbp[CHANNEL_MAX_COUNT];
  for (unsigned int ch = 0; ch < _channel_num; ch++) {
    ch_lbp[ch] = allocate_block(ch, index0, index1);
    if (ch_lbp[ch] == nullptr) {
      _ring_sample_count = align_sample_count + offset - _loop_offset;
      _ch_fraction = 0;
      _dest_ptr = nullptr;
      return;
    }
  }

  // Per-channel source extraction context: sample-interleaved input means
  // sample s channel ch bit = src[s*unitsize + ch/8] bit (ch%8). Precompute
  // byte_pos/bit_mask to avoid recomputing in every phase loop.
  struct ChannelCtx { uint8_t byte_pos; uint8_t bit_mask; };
  ChannelCtx ch_ctx[CHANNEL_MAX_COUNT];
  for (unsigned int ch = 0; ch < _channel_num; ch++) {
    ch_ctx[ch] = {static_cast<uint8_t>(ch / 8),
                  static_cast<uint8_t>(1u << (ch % 8))};
  }

  const uint8_t *src = static_cast<const uint8_t *>(logic.data);
  uint64_t samples_left = num_samples;

  // Leaf-block advancement: calc mipmap for the completed block, advance
  // align_sample_count/index0/index1/offset, allocate the next block.
  // Returns false on out-of-range or allocation failure (caller finalizes
  // and returns). Eliminates 4× duplication of this pattern across phases.
  auto advance_leaf_block = [&]() -> bool {
    if (offset != LeafBlockSamples) return true;
    for (unsigned int ch = 0; ch < _channel_num; ch++)
      calc_mipmap(ch, (uint8_t)index0, (uint8_t)index1, LeafBlockSamples, true);
    align_sample_count += LeafBlockSamples;
    index0 = align_sample_count / LeafBlockSamples / RootScale;
    index1 = (align_sample_count / LeafBlockSamples) % RootScale;
    offset = 0;
    if (index0 >= _ch_data[0].size()) {
      pxv_err("append_payload_impl: index0 %llu out of range (advance)",
              (unsigned long long)index0);
      return false;
    }
    for (unsigned int ch = 0; ch < _channel_num; ch++) {
      ch_lbp[ch] = allocate_block(ch, index0, index1);
      if (ch_lbp[ch] == nullptr) return false;
    }
    return true;
  };

  auto finalize_error = [&]() {
    _ring_sample_count = align_sample_count + offset - _loop_offset;
    _ch_fraction = 0;
    _dest_ptr = nullptr;
  };

  // ---- Phase 1: complete the partial dest byte (bit-level) ----
  // Consumes 1-7 samples to reach a byte boundary (_byte_fraction → 0).
  if (_byte_fraction != 0 && samples_left > 0) {
    const uint8_t need = static_cast<uint8_t>(8 - _byte_fraction);
    const uint64_t have = std::min(samples_left, static_cast<uint64_t>(need));

    // mmap data write — release _mutex to avoid blocking UI during page faults
    lock.unlock();
    {
      const auto pd_w0 = std::chrono::steady_clock::now();
      for (unsigned int ch = 0; ch < _channel_num; ch++) {
        auto [byte_pos, bit_mask] = ch_ctx[ch];
        uint8_t *dest_byte = static_cast<uint8_t *>(ch_lbp[ch]) + offset / 8;
        for (uint64_t k = 0; k < have; k++) {
          if (src[k * unitsize + byte_pos] & bit_mask)
            *dest_byte |= static_cast<uint8_t>(1u << (_byte_fraction + k));
        }
      }
      pd_write_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - pd_w0).count();
    }
    lock.lock();  // re-acquire for metadata (advance_leaf_block)

    src += have * unitsize;
    samples_left -= have;
    offset += have;
    _byte_fraction = static_cast<uint8_t>((_byte_fraction + have) % 8);

    if (!advance_leaf_block()) { finalize_error(); return; }
  }

  // ---- Phase 2: align offset to Scale boundary via 8-sample byte writes ----
  // ROOT-CAUSE FIX for D7 data-loss bug. The original code placed Phase 3
  // (64-sample batch) directly after Phase 1, but Phase 1 only guarantees
  // offset % 8 == 0, not offset % Scale == 0. Phase 3 used `offset / Scale`
  // (integer division) to compute the write pointer, causing bit-misalignment
  // when offset % Scale != 0: write_ptr = u64[offset/Scale] maps bit 0 to
  // sample (offset/Scale)*Scale, but the value's bit 0 corresponds to sample
  // `offset`. The misaligned overwrite corrupted neighboring u64s, producing
  // symptoms like u64[3494]=0xffffff0000000000 (40 zero bits = 200us @200kHz).
  // This new phase strengthens the alignment invariant: it consumes 8-sample
  // bytes until offset % Scale == 0, so Phase 3's alignment assumption becomes
  // a guaranteed invariant rather than an unchecked hypothesis.
  while (samples_left >= 8 && (offset % Scale) != 0) {
    // mmap data write — release _mutex during page faults
    lock.unlock();
    {
      const auto pd_w0 = std::chrono::steady_clock::now();
      for (unsigned int ch = 0; ch < _channel_num; ch++) {
        auto [byte_pos, bit_mask] = ch_ctx[ch];
        uint8_t *dest_byte = static_cast<uint8_t *>(ch_lbp[ch]) + offset / 8;
        uint8_t byte = 0;
        for (uint8_t k = 0; k < 8; k++) {
          if (src[k * unitsize + byte_pos] & bit_mask)
            byte |= static_cast<uint8_t>(1u << k);
        }
        *dest_byte = byte;
      }
      pd_write_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - pd_w0).count();
    }
    lock.lock();  // re-acquire for metadata (advance_leaf_block)

    src += 8 * unitsize;
    samples_left -= 8;
    offset += 8;

    if (!advance_leaf_block()) { finalize_error(); return; }
  }

  // ---- Phase 3: process 64-sample batches (Scale-aligned fast path) ----
  // INVARIANT: offset % Scale == 0, guaranteed by Phase 2. write_ptr bit 0
  // correctly maps to sample offset. No integer-division misalignment.
  while (samples_left >= Scale && _byte_fraction == 0) {
    // mmap data write — release _mutex during page faults
    lock.unlock();
    {
      const auto pd_w0 = std::chrono::steady_clock::now();
      for (unsigned int ch = 0; ch < _channel_num; ch++) {
        auto [byte_pos, bit_mask] = ch_ctx[ch];
        uint64_t *write_ptr =
            reinterpret_cast<uint64_t *>(ch_lbp[ch]) + offset / Scale;
        uint64_t value = 0;
        for (unsigned int m = 0; m < ScaleSize; m++) {
          uint8_t byte = 0;
          for (unsigned int k = 0; k < 8; k++) {
            if (src[(m * 8 + k) * unitsize + byte_pos] & bit_mask)
              byte |= static_cast<uint8_t>(1u << k);
          }
          value |= static_cast<uint64_t>(byte) << (m * 8);
        }
        *write_ptr = value;
      }
      pd_write_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - pd_w0).count();
    }
    lock.lock();  // re-acquire for metadata (advance_leaf_block)

    src += Scale * unitsize;
    samples_left -= Scale;
    offset += Scale;

    if (!advance_leaf_block()) { finalize_error(); return; }
  }

  // ---- Phase 4: process remaining 8-sample bytes ----
  while (samples_left >= 8 && _byte_fraction == 0) {
    // mmap data write — release _mutex during page faults
    lock.unlock();
    {
      const auto pd_w0 = std::chrono::steady_clock::now();
      for (unsigned int ch = 0; ch < _channel_num; ch++) {
        auto [byte_pos, bit_mask] = ch_ctx[ch];
        uint8_t *dest_byte = static_cast<uint8_t *>(ch_lbp[ch]) + offset / 8;
        uint8_t byte = 0;
        for (uint8_t k = 0; k < 8; k++) {
          if (src[k * unitsize + byte_pos] & bit_mask)
            byte |= static_cast<uint8_t>(1u << k);
        }
        *dest_byte = byte;
      }
      pd_write_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - pd_w0).count();
    }
    lock.lock();  // re-acquire for metadata (advance_leaf_block)

    src += 8 * unitsize;
    samples_left -= 8;
    offset += 8;

    if (!advance_leaf_block()) { finalize_error(); return; }
  }

  // ---- Phase 5: residual 1-7 samples (bit-level, persist _byte_fraction) ----
  if (samples_left > 0 && _byte_fraction == 0) {
    // mmap data write — release _mutex during page faults
    lock.unlock();
    {
      const auto pd_w0 = std::chrono::steady_clock::now();
      for (unsigned int ch = 0; ch < _channel_num; ch++) {
        auto [byte_pos, bit_mask] = ch_ctx[ch];
        uint8_t *dest_byte = static_cast<uint8_t *>(ch_lbp[ch]) + offset / 8;
        for (uint64_t k = 0; k < samples_left; k++) {
          if (src[k * unitsize + byte_pos] & bit_mask)
            *dest_byte |= static_cast<uint8_t>(1u << k);
        }
      }
      pd_write_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - pd_w0).count();
    }
    lock.lock();  // re-acquire for mipmap calc & finalize

    _byte_fraction = static_cast<uint8_t>(samples_left);
    offset += samples_left;
    samples_left = 0;
  }

  // ---- Mipmap calc for partial leaf block (incremental rendering) ----
  // calc_mipmap processes only complete 64-sample (Scale) chunks via integer
  // division (samples / Scale), so partial bytes at the tail are not touched.
  // _last_calc_count is saved (isEnd=false) for continuation on the next call.
  if (offset > 0 && offset < LeafBlockSamples) {
    for (unsigned int ch = 0; ch < _channel_num; ch++)
      calc_mipmap(ch, (uint8_t)index0, (uint8_t)index1, offset, false);
  }

  // Finalize position
  const uint64_t absolute_position = align_sample_count + offset;
  _ring_sample_count = absolute_position - _loop_offset;

  // Loop-mode overflow handling. Non-loop: dead code due to num_samples cap
  // above, but the explicit _is_loop guard defends against future regressions.
  if (_is_loop && absolute_position > _total_sample_count) {
    _loop_offset = absolute_position - _total_sample_count;
    _ring_sample_count = _total_sample_count;
  }

  // C3: publish committed count (release) for lock-free finite readers.
  // calc_mipmap for the region completed above (the `calc_mipmap` calls
  // happen before _ring_sample_count is finalized), so any reader that
  // acquire-loads `_ring_published` sees consistent metadata + leaf data.
  if (!_is_loop)
    _ring_published.store(_ring_sample_count, std::memory_order_release);

  _ch_fraction = 0;
  _dest_ptr = nullptr;
}

void LogicSnapshot::append_cross_payload(const sr_datafeed_logic &logic) {
  // Raw channel-block (LA_CROSS_DATA) input — v1.49 bit-copy algorithm.
  // Hardware DMA layout: 64 samples for ch0, 64 samples for ch1, ...,
  // 64 samples for chN-1, then repeat. Each "chunk" is (channel_num * 8)
  // bytes = (channel_num * 64) samples.
  //
  // The per-channel chunk tree stores each channel's samples independently
  // (8 samples per byte, LeafBlockSamples=65536 samples per leaf block).
  // Because the input is already channel-separated (just interleaved in
  // 64-sample blocks), we can bit-copy each channel's 8-byte u64 directly
  // into the destination — no per-bit deinterleave needed. This is ~20x
  // faster than the sample-interleaved path (append_payload_impl).
  //
  // State machine (continued across calls):
  //   _ch_fraction:   current channel index within the 64-sample chunk
  //   _byte_fraction: bit offset within the current 8-byte u64 (0..7)
  //   _dest_ptr:      next byte to write in the current leaf block
  //   _ring_sample_count: per-channel sample count (advances by Scale=64
  //                       each time we complete one channel's chunk)
  //
  // See v1.49 PXView/src/data/logicsnapshot.cpp:208 for the original.

  assert(logic.format == LA_CROSS_DATA);
  assert(logic.data);
  assert(_channel_num > 0);

  // Defensive: assert() is a no-op in Release — guard against null data.
  if (!logic.data || _channel_num == 0) return;

  if (logic.length == 0) return;

  // Cross data must be chunk-aligned: each chunk is (channel_num * 8) bytes
  // (64 samples per channel × 8 bytes per 64 samples / 8 samples per byte).
  // The async worker already truncates length to a multiple of chunk_size,
  // but assert defensively.
  const uint64_t chunk_size = (uint64_t)_channel_num * 8;
  if (logic.length < chunk_size) return;

  uint8_t *data_src_ptr = (uint8_t *)logic.data;
  uint64_t len = logic.length;
  uint64_t index0 = 0;
  uint64_t index1 = 0;
  uint64_t offset = 0;
  void *lbp = nullptr;

  // samples = total samples per channel in this packet
  uint64_t samples = (logic.length * 8) / _channel_num;

  // [PathDiag] 入口计时起点 (含 _mutex 获取等待 — 唯一未被写块/元数据覆盖的盲区)
  static PathDiagWindow pd_win;
  static uint64_t pd_seq = 0;
  const auto pd_entry = std::chrono::steady_clock::now();
  double pd_lockwait_ms = 0.0, pd_write_ms = 0.0;
  // RAII 汇总器: 函数所有 return 路径统一记账 (只统计真正处理的 payload)
  struct PdFinalize {
    PathDiagWindow &w; uint64_t &seq; uint64_t bytes;
    std::chrono::steady_clock::time_point t0;
    double &lw; double &wr;
    ~PdFinalize() {
      const double total = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0).count();
      w.add(++seq, bytes, total, lw, wr);
    }
  } pd_fin{pd_win, pd_seq, (uint64_t)logic.length, pd_entry, pd_lockwait_ms, pd_write_ms};

  // Segmented locking: metadata ops (allocate_block, _sample_count/
  // _ring_sample_count updates, calc_mipmap, loop housekeeping) hold _mutex;
  // mmap data writes (page-fault-prone) release it so UI's get_samples can
  // proceed. _dest_ptr/_ch_fraction/_byte_fraction are async-writer-private
  // and safe to touch without the lock during write loops.
  std::unique_lock<std::recursive_mutex> lock(_mutex);
  pd_lockwait_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - pd_entry).count();

  // Update _sample_count (cap at _total_sample_count)
  if (_sample_count + samples < _total_sample_count) {
    _sample_count += samples;
  } else {
    if (_sample_count == _total_sample_count && !_is_loop)
      return;
    _sample_count = _total_sample_count;
  }

  // Loop mode housekeeping (same as append_payload_impl)
  if (_is_loop) {
    if (_loop_offset >= LeafBlockSamples * Scale) {
      move_first_node_to_last();
      _loop_offset -= LeafBlockSamples * Scale;
      _lst_free_block_index = 0;
      // move_first_node_to_last freed all Scale leaf blocks in root node 0
      // via push_to_free_list -> decommit_block. If _dest_ptr pointed into
      // any of those blocks it is now dangling. Invalidate bit-align state
      // so the bit-align phase is skipped (the freed data is discarded in
      // loop mode anyway).
      _dest_ptr = nullptr;
      _ch_fraction = 0;
      _byte_fraction = 0;
    } else {
      int free_count = _loop_offset / LeafBlockSamples;
      if (free_count > _lst_free_block_index) {
        free_head_blocks(free_count);
        // free_head_blocks decommits specific leaf blocks in root node 0.
        // _dest_ptr may point into one of them. Invalidate bit-align state.
        _dest_ptr = nullptr;
        _ch_fraction = 0;
        _byte_fraction = 0;
      }
    }
  }

  _ring_sample_count += _loop_offset;

  // ---- Bit-align phase: complete partial u64 from previous call ----
  // Driven by _ch_fraction (channel) and _byte_fraction (bit 0..7 within
  // the current 8-byte u64 for that channel).
  // Safety: _dest_ptr is set to nullptr whenever blocks are freed or
  // decommitted (see push_to_free_list, move_first_node_to_last,
  // free_head_blocks, first_payload). The prefault thread's decommit
  // logic has been removed, so mmap-backed pages cannot be decommitted
  // while data is being collected. The null check below is the primary
  // defense against use-after-free of decommitted/reclaimed memory.
  while ((_ch_fraction != 0 || _byte_fraction != 0) && len > 0) {
    if (_dest_ptr == nullptr) {
      // ROOT-CAUSE FIX (stream/trigger live capture): a previous append that
      // early-returned (out-of-range / alloc-fail) left `_dest_ptr == nullptr`
      // WITHOUT resetting `_ch_fraction`/`_byte_fraction`. The stale residue
      // then drives every subsequent payload into this branch, and a plain
      // `return` here drops the WHOLE payload forever → capture ends with
      // ~no data (_ring≈0). `_dest_ptr==nullptr` means the carried bit-align
      // position is invalid/unrecoverable, so the correct behavior is to drop
      // only the <1-chunk partial alignment and re-anchor to the (still
      // Scale-aligned) `_ring_sample_count`, then continue the main path.
      // `_ring_sample_count` is always Scale(64)-aligned here — bit-align only
      // advances whole chunks and tracks the intra-chunk residue in
      // `_ch_fraction`/`_byte_fraction` — so the main-path invariants hold.
      pxv_warn("append_cross_payload: _dest_ptr nullptr during bit-align, "
               "resetting residue (ch_fraction=%u byte_fraction=%u)",
               (unsigned)_ch_fraction, (unsigned)_byte_fraction);
      _dest_ptr = nullptr;
      _ch_fraction = 0;
      _byte_fraction = 0;
      break;
    }

    // mmap data write — release _mutex during page faults
    lock.unlock();
    {
      const auto pd_w0 = std::chrono::steady_clock::now();
      do {
        *_dest_ptr++ = *data_src_ptr++;
        _byte_fraction = (_byte_fraction + 1) % 8;
        len--;
      } while (_byte_fraction != 0 && len > 0);
      pd_write_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - pd_w0).count();
    }
    lock.lock();  // re-acquire for metadata (allocate_block/calc_mipmap)

    if (_byte_fraction == 0) {
      index0 = _ring_sample_count / LeafBlockSamples / RootScale;
      index1 = (_ring_sample_count / LeafBlockSamples) % RootScale;
      offset = (_ring_sample_count % LeafBlockSamples) / 8;

      _ch_fraction = (_ch_fraction + 1) % _channel_num;

      if (index0 >= _ch_data[_ch_fraction].size()) {
        pxv_err("append_cross_payload: index0 %llu out of range (bit-align)",
                (unsigned long long)index0);
        _ring_sample_count -= _loop_offset;
        _dest_ptr = nullptr;
        return;
      }
      lbp = allocate_block(_ch_fraction, (uint16_t)index0, (uint16_t)index1);
      if (lbp == nullptr) {
        pxv_err("append_cross_payload: alloc failed (bit-align)");
        _ring_sample_count -= _loop_offset;
        _dest_ptr = nullptr;
        return;
      }

      _dest_ptr = (uint8_t *)lbp + offset;

      // Completed one channel's 64-sample chunk → advance ring count
      if (_ch_fraction == 0) {
        _ring_sample_count += Scale;

        if (_ring_sample_count % LeafBlockSamples == 0) {
          calc_mipmap(_channel_num - 1, (uint8_t)index0, (uint8_t)index1,
                      LeafBlockSamples, true);
        }
        break;
      }
    }
  }

  // ---- Main phase: chunk-aligned fast path ----
  // INVARIANT: _ch_fraction == 0, _byte_fraction == 0, _ring_sample_count % Scale == 0
  assert(_ch_fraction == 0);
  assert(_byte_fraction == 0);
  assert(_ring_sample_count % Scale == 0);

  uint64_t align_sample_count = _ring_sample_count;
  // chunk_size already declared above (line ~1067) — reuse it.
  // Shared across the blocked-transpose fast path and the per-channel
  // fallback below; consumed by the shared tail/finalize section.
  void *end_read_ptr = (uint8_t *)data_src_ptr + len;
  uint16_t last_chan = _ch_fraction;

  // C1 (P9-on-raw): blocked-transpose main phase. The original loop performed
  // one strided re-scan of the payload per channel (read_ptr += _channel_num
  // every u64 → 256B stride @32ch → the whole payload is read once per
  // channel = Nx read amplification → ingest ~1620 MB/s, below the 2400 MB/s
  // gate). Replace with a single sequential pass that transposes each chunk
  // into the per-channel leaf blocks: contiguous input reads + contiguous
  // per-channel writes ≈ memcpy bandwidth. Taken only when the payload is
  // chunk-aligned AND we're at a chunk boundary (no pending partial u64) —
  // the demo/hw always send chunk-aligned payloads; anything else falls back
  // to the original per-channel loop below (identical semantics).
  if (len >= chunk_size && (len % chunk_size == 0) &&
      _ch_fraction == 0 && _byte_fraction == 0) {
    uint64_t idx0 = align_sample_count / LeafBlockSamples / RootScale;
    uint64_t idx1 = (align_sample_count / LeafBlockSamples) % RootScale;
    uint64_t blk_off = align_sample_count % LeafBlockSamples;

    // Per-channel destination u64 pointers into their current leaf blocks.
    // All channels share the same sample position in CROSS format.
    uint64_t *dst[CHANNEL_MAX_COUNT];
    for (uint64_t c = 0; c < _channel_num; c++) {
      if (idx0 >= _ch_data[c].size()) {
        pxv_err("append_cross_payload: idx0 %llu out of range (blocked)",
                (unsigned long long)idx0);
        _ring_sample_count = align_sample_count - _loop_offset;
        _dest_ptr = nullptr;
        return;
      }
      void *blk = allocate_block(c, (uint16_t)idx0, (uint16_t)idx1);
      if (blk == nullptr) {
        pxv_err("append_cross_payload: alloc failed (blocked)");
        _ring_sample_count = align_sample_count - _loop_offset;
        _dest_ptr = nullptr;
        return;
      }
      dst[c] = (uint64_t *)blk + blk_off / Scale;
    }

    const uint64_t *src = (const uint64_t *)data_src_ptr;
    uint64_t full_chunks = len / chunk_size;
    uint64_t processed = 0;  // samples per channel consumed this payload

    while (full_chunks > 0) {
      // Chunks until the leaf-block boundary (all channels together).
      uint64_t chunks_to_boundary = (LeafBlockSamples - blk_off) / Scale;
      uint64_t nb = std::min(full_chunks, chunks_to_boundary);
      if (nb == 0)
        break;  // defensive: boundary already reached in a prior iteration

      // Lock-free transpose of nb chunks. Data written here is uncommitted
      // and writer-private (readers only see samples released via
      // _ring_published / under _mutex in loop mode) → no lock needed.
      lock.unlock();
      {
        const auto pd_w0 = std::chrono::steady_clock::now();
        for (uint64_t k = 0; k < nb; k++) {
          const uint64_t *chunk = src + k * _channel_num;
          for (uint64_t c = 0; c < _channel_num; c++)
            dst[c][k] = chunk[c];
        }
        pd_write_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - pd_w0).count();
      }
      lock.lock();  // re-acquire for metadata (calc_mipmap/allocate_block)

      processed += nb * Scale;
      blk_off += nb * Scale;
      full_chunks -= nb;
      src += nb * _channel_num;

      if (blk_off == LeafBlockSamples) {
        // All channels completed this leaf block simultaneously → calc
        // mipmap for every channel, then advance all to the next block.
        for (uint64_t c = 0; c < _channel_num; c++)
          calc_mipmap(c, (uint8_t)idx0, (uint8_t)idx1, LeafBlockSamples, true);

        idx1++;
        if (idx1 == RootScale) {
          idx1 = 0;
          idx0++;
        }
        blk_off = 0;

        if (idx0 >= _ch_data[0].size()) {
          pxv_err("append_cross_payload: idx0 %llu out of range (blocked adv)",
                  (unsigned long long)idx0);
          break;
        }
        for (uint64_t c = 0; c < _channel_num; c++) {
          void *blk = allocate_block(c, (uint16_t)idx0, (uint16_t)idx1);
          if (blk == nullptr) {
            pxv_err("append_cross_payload: alloc failed (blocked adv)");
            break;
          }
          dst[c] = (uint64_t *)blk;
        }
      }
    }

    align_sample_count += processed;
    len = 0;  // all full chunks consumed (payload was chunk-aligned)
    index0 = idx0;
    index1 = idx1;
    offset = blk_off;
    last_chan = 0;  // every channel advanced equally → next payload starts at ch0

    // ---- Incremental mipmap for the current partial leaf block ----
    // All channels share the same `blk_off` (cross format) and `index0/index1`
    // here. calc_mipmap processes only complete 64-sample (Scale) chunks via
    // integer division (samples / Scale), so partial bytes at the tail are not
    // touched; _last_calc_count is saved (isEnd=false) for continuation on the
    // next call. Without this, the in-progress leaf block (tog=0, first=0) makes
    // get_sample_self return level 0 for every sample → stream-mode live
    // rendering shows all-LOW until capture_ended() finalizes the tail block.
    if (offset > 0 && offset < LeafBlockSamples) {
      for (unsigned int c = 0; c < _channel_num; c++)
        calc_mipmap(c, (uint8_t)index0, (uint8_t)index1, offset, false);
    }
  } else {
  // ---- Original per-channel strided loop (fallback: non-chunk-aligned
  //      payload, or pending partial u64 from a previous call) ----
  uint64_t *read_ptr = (uint64_t *)data_src_ptr;
  void *end_read_ptr = (uint8_t *)data_src_ptr + len;

  uint64_t filled_sample = align_sample_count % LeafBlockSamples;
  uint64_t old_filled_sample = filled_sample;
  uint64_t *chans_read_addr[CHANNEL_MAX_COUNT];
  for (unsigned int i = 0; i < _channel_num; i++) {
    chans_read_addr[i] = (uint64_t *)data_src_ptr + i;
  }

  uint16_t fill_chan = _ch_fraction;
  last_chan = _ch_fraction;
  index0 = align_sample_count / LeafBlockSamples / RootScale;
  index1 = (align_sample_count / LeafBlockSamples) % RootScale;
  offset = align_sample_count % LeafBlockSamples;

  if (index0 >= _ch_data[fill_chan].size()) {
    pxv_err("append_cross_payload: index0 %llu out of range (main)",
            (unsigned long long)index0);
    _ring_sample_count = align_sample_count - _loop_offset;
    _dest_ptr = nullptr;
    return;
  }
  lbp = allocate_block(fill_chan, (uint16_t)index0, (uint16_t)index1);
  if (lbp == nullptr) {
    pxv_err("append_cross_payload: alloc failed (main)");
    _ring_sample_count = align_sample_count - _loop_offset;
    _dest_ptr = nullptr;
    return;
  }

  uint64_t *write_ptr = (uint64_t *)lbp + offset / Scale;

  while (len >= 8) {
    lock.unlock();
    {
      const auto pd_w0 = std::chrono::steady_clock::now();
      do {
        *write_ptr++ = *read_ptr;
        read_ptr += _channel_num;
        len -= 8;
        filled_sample += Scale;
        last_chan++;
        if (last_chan == _channel_num) {
          last_chan = 0;
        }
      } while (len >= 8 && filled_sample < LeafBlockSamples &&
               read_ptr < end_read_ptr);
      pd_write_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - pd_w0).count();
    }
    lock.lock();  // re-acquire for metadata (calc_mipmap/allocate_block)

    if (filled_sample == LeafBlockSamples) {
      // Completed one leaf block for fill_chan → calc mipmap
      calc_mipmap(fill_chan, (uint8_t)index0, (uint8_t)index1,
                  LeafBlockSamples, true);

      chans_read_addr[fill_chan] = read_ptr;
      fill_chan = (fill_chan + 1) % _channel_num;

      if (fill_chan == 0)
        align_sample_count += (filled_sample - old_filled_sample);

      index0 = align_sample_count / LeafBlockSamples / RootScale;
      index1 = (align_sample_count / LeafBlockSamples) % RootScale;
      offset = align_sample_count % LeafBlockSamples;
      filled_sample = align_sample_count % LeafBlockSamples;
      old_filled_sample = filled_sample;

      if (index0 >= _ch_data[fill_chan].size()) {
        pxv_err("append_cross_payload: index0 %llu out of range (advance)",
                (unsigned long long)index0);
        break;
      }
      lbp = allocate_block(fill_chan, (uint16_t)index0, (uint16_t)index1);
      if (lbp == nullptr) {
        pxv_err("append_cross_payload: alloc failed (advance)");
        break;
      }

      write_ptr = (uint64_t *)lbp + offset / Scale;
      read_ptr = chans_read_addr[fill_chan];
    } else if (read_ptr >= end_read_ptr) {
      // Reached end of input mid-channel → calc partial mipmap
      calc_mipmap(fill_chan, (uint8_t)index0, (uint8_t)index1,
                  filled_sample, false);

      fill_chan = (fill_chan + 1) % _channel_num;

      if (fill_chan == 0)
        align_sample_count += (filled_sample - old_filled_sample);

      index0 = align_sample_count / LeafBlockSamples / RootScale;
      index1 = (align_sample_count / LeafBlockSamples) % RootScale;
      offset = align_sample_count % LeafBlockSamples;
      filled_sample = align_sample_count % LeafBlockSamples;
      old_filled_sample = filled_sample;

      if (index0 >= _ch_data[fill_chan].size()) {
        pxv_err("append_cross_payload: index0 %llu out of range (end)",
                (unsigned long long)index0);
        break;
      }
      lbp = allocate_block(fill_chan, (uint16_t)index0, (uint16_t)index1);
      if (lbp == nullptr) {
        pxv_err("append_cross_payload: alloc failed (end)");
        break;
      }

      write_ptr = (uint64_t *)lbp + offset / Scale;
      read_ptr = chans_read_addr[fill_chan];
    }
  }
  }

  _ring_sample_count = align_sample_count;
  _ring_sample_count -= _loop_offset;

  if (align_sample_count > _total_sample_count) {
    // Loop-mode overflow handling. FINITE (non-loop): the demo driver may
    // overshoot the configured sample limit (e.g. 64-sample chunk alignment:
    // 1,048,064 sent vs 1,000,000 requested) — clamp the ring count but do
    // NOT set `_loop_offset` (loop-only rebase; a non-zero `_loop_offset` in
    // finite mode corrupts get_block_buf/get_block_size/get_samples — the
    // save→load round-trip reads data shifted by _loop_offset/8 bytes).
    // Mirrors append_payload_impl's `_is_loop` guard.
    if (_is_loop)
      _loop_offset = align_sample_count - _total_sample_count;
    _ring_sample_count = _total_sample_count;
  }

  // C3: publish committed count (release) for lock-free finite readers.
  if (!_is_loop)
    _ring_published.store(_ring_sample_count, std::memory_order_release);

  _ch_fraction = last_chan;

  if (index0 >= _ch_data[_ch_fraction].size()) {
    pxv_err("append_cross_payload: index0 %llu out of range (finalize)",
            (unsigned long long)index0);
    _dest_ptr = nullptr;
    return;
  }
  lbp = allocate_block(_ch_fraction, (uint16_t)index0, (uint16_t)index1);
  if (lbp == nullptr) {
    pxv_err("append_cross_payload: alloc failed (finalize)");
    _dest_ptr = nullptr;
    return;
  }

  _dest_ptr = (uint8_t *)lbp + offset / 8;

  // ---- Tail phase: residual bytes (len < 8) ----
  if (len > 0) {
    if (_dest_ptr == nullptr) {
      pxv_err("append_cross_payload: _dest_ptr nullptr in tail phase, "
              "dropping %llu residual bytes", (unsigned long long)len);
      return;
    }
    uint8_t *src_ptr = (uint8_t *)end_read_ptr - len;
    _byte_fraction += (uint8_t)len;

    // mmap data write — release _mutex during page faults
    lock.unlock();
    {
      const auto pd_w0 = std::chrono::steady_clock::now();
      while (len > 0) {
        *_dest_ptr++ = *src_ptr++;
        len--;
      }
      pd_write_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - pd_w0).count();
    }
    lock.lock();  // re-acquire before method exit (destructor releases)
  }
}

void LogicSnapshot::capture_ended() {
  // CRITICAL FIX: Drain the async write queue BEFORE acquiring _mutex.
  // Without this, _ring_sample_count may be stale (the async writer hasn't
  // finished writing all pending data), and the memset below would zero out
  // valid data that was still waiting in the queue.
  // We must NOT hold _mutex while waiting, because the async worker needs
  // _mutex to call append_payload_impl().
  // Encapsulated in drain_queue_for_capture_end() (cluster D).
  _disk_cache_writer->drain_queue_for_capture_end();

  std::lock_guard<std::recursive_mutex> lock(_mutex);

  Snapshot::capture_ended();

  _sample_count = _ring_sample_count;
  _ring_sample_count += _loop_offset;

  uint64_t index0 = _ring_sample_count / LeafBlockSamples / RootScale;
  uint64_t index1 = (_ring_sample_count / LeafBlockSamples) % RootScale;
  uint64_t offset = (_ring_sample_count % LeafBlockSamples) / 8;

  _ring_sample_count -= _loop_offset;

  pxv_info("capture_ended: _ring=%llu, _total=%llu, _sample_count=%llu, "
           "_loop_offset=%llu, _ch_num=%u, _ch_data.size=%zu, "
           "idx0=%llu, idx1=%llu, offset=%llu",
           (unsigned long long)_ring_sample_count,
           (unsigned long long)_total_sample_count,
           (unsigned long long)_sample_count,
           (unsigned long long)_loop_offset,
           (unsigned)_channel_num, _ch_data.size(),
           (unsigned long long)index0, (unsigned long long)index1,
           (unsigned long long)offset);

  if (offset > 0) {
    for (unsigned int chan = 0; chan < _channel_num; chan++) {
      uint8_t *lbp = (uint8_t *)_ch_data[chan][index0].lbp[index1];

      if (lbp == nullptr) {
        // Tolerate nullptr leaf block at capture_ended: the async writer may not
        // have allocated a block for the trailing partial chunk (e.g. when the
        // last append_payload_impl invocation early-returned because
        // _sample_count already capped at _total_sample_count). Previously
        // this fired assert(false); but per AGENTS.md assert() is a no-op in
        // Release, so we already log + skip silently here. Skip the mipmap
        // update for this channel — the trailing partial chunk's mipmap will
        // be 0 anyway, and downstream view rendering already tolerates it.
        pxv_warn("capture_ended: ch%u leaf block [%llu][%llu] is nullptr, skipping",
                 chan, (unsigned long long)index0, (unsigned long long)index1);
      } else {
        // ONLY clear the signal data part, NOT the mipmaps! Mipmaps start at LeafBlockSamples / 8.
        if (offset < LeafBlockSamples / 8) {
            memset(lbp + offset, 0, (LeafBlockSamples / 8) - offset);
        }
        calc_mipmap(chan, index0, index1, offset * 8, true);
      }
    }
  }

}

void LogicSnapshot::copy_from(const LogicSnapshot &src) {
std::lock_guard<std::recursive_mutex> lock(_mutex);

// H4 fix: drain the async writer before free_data() resets _mmap_alloc.
// Without this, the async worker could still be accessing the old mmap
// allocator through _owner->_mmap_alloc while free_data() resets it.
_disk_cache_writer->drain_and_join();

const_cast<LogicSnapshot &>(src).ensure_all_blocks_hot();

free_data();

  _capacity = src._capacity;
  _channel_num = src._channel_num;
  _sample_count = src._sample_count;
  _total_sample_count = src._total_sample_count;
  _ring_sample_count = src._ring_sample_count;
  _unit_size = src._unit_size;
  _unit_bytes = src._unit_bytes;
  _unit_pitch = src._unit_pitch;
  _memory_failed = src._memory_failed.load();
  _last_ended = src._last_ended.load();
  _samplerate = src._samplerate.load();
  _ch_index = src._ch_index;

  _byte_fraction = src._byte_fraction;
  _ch_fraction = src._ch_fraction;
  _dest_ptr = nullptr;
  memcpy(_last_sample, src._last_sample, sizeof(_last_sample));
  memcpy(_last_calc_count, src._last_calc_count, sizeof(_last_calc_count));
  _is_loop = src._is_loop;
  _loop_offset = src._loop_offset;
  _able_free = src._able_free;
  memcpy(_cur_ref_block_indexs, src._cur_ref_block_indexs,
         sizeof(_cur_ref_block_indexs));
  _lst_free_block_index = src._lst_free_block_index;

  _max_blocks_per_channel = src._max_blocks_per_channel;

  if (src._mmap_alloc) {
      _mmap_alloc = std::make_shared<MmapAllocator>();
      _mmap_alloc->configure(false, "", src._mmap_alloc->get_total_bytes(),
                             LeafBlockSpace, _max_blocks_per_channel, _channel_num);
      // CRITICAL FIX: stop the prefault thread immediately after configure().
      // configure() spawns a background thread that writes zero bytes to every
      // page in the mmap region to pre-fault them into RAM.  That thread races
      // with the memcpy loop below: if it reaches a page AFTER memcpy has
      // already written real data there, it overwrites the first byte of that
      // page with 0, silently corrupting both sample data and mipmap data.
      // This was the root cause of "first ~2ms of both channels show phantom
      // waveform after applying glitch filter to a second channel": the second
      // filter call invokes copy_from() to restore from backup, the prefault
      // thread corrupts the restored data, and the glitch filter then operates
      // on corrupted data producing wrong results for BOTH channels.
      _mmap_alloc->stop_prefault();
  } else {
      _disk_cache_writer->clear_all_mmap_slots();
      _mmap_alloc = nullptr;
  }

  for (size_t i = 0; i < src._ch_data.size(); i++) {
    std::vector<struct RootNode> new_channel;
    for (size_t j = 0; j < src._ch_data[i].size(); j++) {
      const RootNode &rn = src._ch_data[i][j];
      RootNode new_rn;
      new_rn.tog = rn.tog;
      new_rn.first = rn.first;
      new_rn.last = rn.last;
      for (unsigned int k = 0; k < Scale; k++) {
        if (rn.lbp[k] != nullptr) {
          if (_mmap_alloc && src._mmap_alloc && src._mmap_alloc->is_mmap_address(rn.lbp[k])) {
            uint64_t global_block_seq = j * RootScale + k;
            void* new_lbp = _mmap_alloc->get_block_data(i, global_block_seq, _max_blocks_per_channel, LeafBlockSpace);
            if (new_lbp) {
                memcpy(new_lbp, rn.lbp[k], LeafBlockSpace);
            } else {
                _memory_failed = true;
            }
            new_rn.lbp[k] = new_lbp;
          } else {
            new_rn.lbp[k] = LeafBlockPool::instance().acquire(LeafBlockSpace);
            if (new_rn.lbp[k])
              memcpy(new_rn.lbp[k], rn.lbp[k], LeafBlockSpace);
            else
              _memory_failed = true;
          }
        } else {
          new_rn.lbp[k] = nullptr;
        }
      }
      new_channel.push_back(new_rn);
    }
    _ch_data.push_back(std::move(new_channel));
  }
}

void LogicSnapshot::calc_mipmap(unsigned int order, uint8_t index0,
                                uint8_t index1, uint64_t samples, bool isEnd) {
  void *lbp = _ch_data[order][index0].lbp[index1];

  if (lbp == nullptr)
    return;
  void *level1_ptr = (uint8_t *)lbp + LeafBlockSamples / 8;
  void *level2_ptr = (uint8_t *)level1_ptr + LeafBlockSamples / Scale / 8;
  void *level3_ptr =
      (uint8_t *)level2_ptr + LeafBlockSamples / Scale / Scale / 8;

  // ROOT-CAUSE FIX for D7 false-edge bug (160-210us spurious low pulses):
  // The original level-1 loop processed every u64 with index < samples/Scale
  // (integer division). When samples % Scale != 0, the last u64 was only
  // partially written (Phase 4/5 wrote the low N bits, the high 64-N bits
  // remained zero from memset). Comparing this partial u64 against
  // _last_sample (which was ~0ULL for D7-all-high) triggered a false toggle,
  // polluting level1/2/3 mipmap and causing get_nxt_edge_self to report
  // phantom edges. The rendering drew 160-210us low pulses (N=32..42 samples
  // at 200kHz) even though get_sample_self correctly returned true for every
  // bit in the written range.
  //
  // Fix: split the loop into a full-u64 phase (i < full_u64_count) and a
  // partial-u64 tail phase. The tail phase masks both the comparison and the
  // _last_sample update to only the actually-written bits, so a partial u64
  // whose written bits all match _last_sample does NOT trigger a toggle.
  const uint64_t full_u64_count = samples / Scale;
  const uint64_t partial_bits = samples % Scale;
  const uint64_t partial_mask =
      partial_bits ? ((1ULL << partial_bits) - 1) : 0ULL;

  // level 1
  uint64_t *src_ptr = (uint64_t *)lbp;
  uint64_t *dest_ptr = (uint64_t *)level1_ptr;
  uint8_t offset = 0;
  uint64_t i = 0;
  uint64_t last_count = _last_calc_count[order];

  if (last_count > 0) {
    i = last_count / Scale;
    offset = i % Scale;
    src_ptr += i;
    dest_ptr += i / Scale;
  }

  if (i == 0) {
    _last_sample[order] = (*src_ptr & LSB) ? ~0ULL : 0ULL;
  }

  // Full u64 phase: every u64 in [i, full_u64_count) is fully written.
  for (; i < full_u64_count; i++) {
    if (_last_sample[order] ^ *src_ptr)
      *dest_ptr |= (1ULL << offset);

    _last_sample[order] = *src_ptr & MSB ? ~0ULL : 0ULL;
    src_ptr++;
    offset++;

    if (offset == Scale) {
      offset = 0;
      dest_ptr++;
    }
  }

  // Partial tail phase: the u64 at index full_u64_count has only its low
  // partial_bits written; high bits are zero. Compare only the written bits
  // and derive _last_sample from the last written bit, not from MSB.
  if (partial_bits > 0) {
    const uint64_t partial_value = *src_ptr & partial_mask;
    const uint64_t last_written_bit =
        (partial_bits > 0) ? (1ULL << (partial_bits - 1)) : 0ULL;
    if ((_last_sample[order] & partial_mask) ^ partial_value)
      *dest_ptr |= (1ULL << offset);

    _last_sample[order] = (partial_value & last_written_bit) ? ~0ULL : 0ULL;
    // NOTE: do NOT advance src_ptr/offset/dest_ptr here — the partial u64 is
    // revisited on the next calc_mipmap call (last_count = samples, so
    // i = samples/Scale = full_u64_count, reprocessing this u64 as a full
    // u64 once its remaining bits are written). Advancing would skip it.
  }

  // level 2
  src_ptr = (uint64_t *)level1_ptr;
  dest_ptr = (uint64_t *)level2_ptr;
  offset = 0;
  i = 0;

  if (last_count > 0) {
    i = last_count / Scale / Scale;
    offset = i % Scale;
    src_ptr += i;
    dest_ptr += i / Scale;
  }

  for (; i < LeafBlockSamples / Scale / Scale; i++) {
    if (*src_ptr)
      *dest_ptr |= (1ULL << offset);

    src_ptr++;
    offset++;

    if (offset == Scale) {
      offset = 0;
      dest_ptr++;
    }
  }

  // level 3
  src_ptr = (uint64_t *)level2_ptr;
  dest_ptr = (uint64_t *)level3_ptr;

  for (i = 0; i < Scale; i++) {
    if (*src_ptr)
      *dest_ptr |= (1ULL << i);
    src_ptr++;
  }

  if ((*((uint64_t *)lbp) & LSB) != 0)
    _ch_data[order][index0].first |= 1ULL << index1;

  // ROOT-CAUSE FIX: the original code checked a fixed u64 position
  // (LeafBlockSamples/Scale - 1 = last u64 of the leaf block). When the leaf
  // block was not fully written (samples < LeafBlockSamples), that u64 was
  // zero-filled memory, so `last` was wrongly cleared to 0. Now derive `last`
  // from the actual last written sample: if partial_bits > 0, the last
  // written bit is bit (partial_bits-1) of u64[full_u64_count]; otherwise
  // it's MSB of u64[full_u64_count - 1].
  if (samples > 0) {
    bool last_sample_value;
    if (partial_bits > 0) {
      const uint64_t tail_u64 = *((uint64_t *)lbp + full_u64_count);
      last_sample_value = (tail_u64 & (1ULL << (partial_bits - 1))) != 0;
    } else {
      const uint64_t last_full_u64 = *((uint64_t *)lbp + full_u64_count - 1);
      last_sample_value = (last_full_u64 & MSB) != 0;
    }
    if (last_sample_value)
      _ch_data[order][index0].last |= 1ULL << index1;
    else
      _ch_data[order][index0].last &= ~(1ULL << index1);
  }

  if (*((uint64_t *)level3_ptr) != 0) {
    _ch_data[order][index0].tog |= 1ULL << index1;
  } else if (isEnd && _able_free) {
    // Only free (decommit) constant-value leaf blocks when _able_free is
    // true. When false (repeat mode, decoder still running on the same
    // shared snapshot), the decoder thread holds raw pointers (di->inbuf)
    // into these mmap leaf blocks. Decommitting them via push_to_free_list
    // → VirtualFree(MEM_DECOMMIT) would cause SIGSEGV in the decoder thread
    // (instance.c:update_old_pins_array). Instead, keep the block mapped;
    // allocate_block() will reuse it in-place for the next capture.
    push_to_free_list(_ch_data[order][index0].lbp[index1]);

    _ch_data[order][index0].lbp[index1] = nullptr;
  }

  if (isEnd)
    _last_calc_count[order] = 0;
  else
    _last_calc_count[order] = samples;
}

const uint8_t *LogicSnapshot::get_samples(uint64_t start_sample,
                                          uint64_t &end_sample, int sig_index,
                                          void **lbp) {
  // C3 (P9-on-raw): FINITE (non-loop) fast path is lock-free — reads only the
  // committed sample range (`_ring_published`, release-published after mipmap
  // completes) from the immutable non-loop tree. Loop/∞ mode keeps the lock
  // (it rotates + frees blocks and rebases via `_loop_offset`).
  const bool looped = _is_loop;
  std::unique_lock<std::recursive_mutex> lock(_mutex, std::defer_lock);
  if (looped)
    lock.lock();

  // P1-6 fix: Increment iterator count so that free_data/free_head_blocks
  // will skip memory optimization while this pointer is in use.
  IteratorGuard iter_guard(this);

  uint64_t sample_count =
      looped ? _ring_sample_count
             : _ring_published.load(std::memory_order_acquire);

  // Guard: sample_count may be 0 when opening a saved waveform file
  // (data not fully loaded or race condition). Original asserts crash via
  // MSVC assertion dialog.
  if (sample_count == 0 || start_sample >= sample_count) {
    pxv_warn("LogicSnapshot::get_samples: sample_count=%llu start=%llu, skipping",
             (unsigned long long)sample_count, (unsigned long long)start_sample);
    return nullptr;
  }

  if (end_sample >= sample_count)
    end_sample = sample_count - 1;

  if (start_sample > end_sample) {
    pxv_warn("LogicSnapshot::get_samples: start=%llu > end=%llu, skipping",
             (unsigned long long)start_sample, (unsigned long long)end_sample);
    return nullptr;
  }

  if (looped)
    start_sample += _loop_offset;
  // Note: previously _ring_sample_count was temporarily modified here
  // (_ring_sample_count += _loop_offset) and restored later. This was
  // unnecessary — all calculations use the local `sample_count` captured
  // above — and dangerous: early-return paths skipped the restoration,
  // permanently corrupting _ring_sample_count. Removed.

  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size()) {
    static int s_warn_cnt = 0;
    if (s_warn_cnt++ < 20) {
      pxv_warn("LogicSnapshot::get_samples nullptr: sig_index=%d order=%d "
               "ch_data_size=%zu ch_index_size=%zu _loop_offset=%lld",
               sig_index, order, _ch_data.size(), _ch_index.size(),
               (long long)_loop_offset);
    }
    return nullptr;
  }

  uint64_t index0 = start_sample >> (LeafBlockPower + RootScalePower);
  uint64_t index1 = (start_sample & RootMask) >> LeafBlockPower;
  uint64_t offset = (start_sample & LeafMask) / 8;

  end_sample = (index0 << (LeafBlockPower + RootScalePower)) +
               (index1 << LeafBlockPower) + ~(~0ULL << LeafBlockPower);

  end_sample = min(end_sample + 1, sample_count);

  if (index0 >= _ch_data[order].size()) {
    static int s_warn_cnt2 = 0;
    if (s_warn_cnt2++ < 20) {
      pxv_warn("LogicSnapshot::get_samples nullptr: sig_index=%d order=%d "
               "index0=%llu >= ch_data[order].size()=%zu (start_sample=%llu)",
               sig_index, order, (unsigned long long)index0,
               _ch_data[order].size(), (unsigned long long)start_sample);
    }
    return nullptr;
  }

  void *ptr = _ch_data[order][index0].lbp[index1];

  if (ptr == nullptr) {
    // Leaf block was freed by calc_mipmap because this region has no toggles
    // (constant value). The value is encoded in _ch_data[order][index0].first
    // bit `index1`. Return a synthetic buffer filled with the constant value
    // so callers (export, etc.) get valid data instead of nullptr.
    // 8 samples per byte, LSB-first: all-0 -> 0x00, all-1 -> 0xFF.
    bool const_val = (_ch_data[order][index0].first & (1ULL << index1)) != 0;
    uint64_t bytes_from_offset = (end_sample - start_sample + 1 + 7) / 8;
    // Clamp to remaining bytes in this leaf block from `offset`.
    uint64_t leaf_remaining = (LeafBlockSamples / 8) - offset;
    uint64_t need = std::min(bytes_from_offset, leaf_remaining);
    static thread_local std::vector<uint8_t> s_const_buf;
    if (s_const_buf.size() < need) s_const_buf.resize(need);
    uint8_t fill = const_val ? 0xFF : 0x00;
    memset(s_const_buf.data(), fill, need);
    if (lbp != nullptr)
      *lbp = nullptr;
    if (looped) {
      _cur_ref_block_indexs[order].root_index = index0;
      _cur_ref_block_indexs[order].lbp_index = index1;
    }
    return s_const_buf.data();
  }

  if (lbp != nullptr)
    *lbp = ptr;

  if (looped) {
    _cur_ref_block_indexs[order].root_index = index0;
    _cur_ref_block_indexs[order].lbp_index = index1;
  }

  return (uint8_t *)ptr + offset;
}

// P1-B: Segment data iterator protocol implementation.
// These methods provide chunk-level contiguous memory access, matching
// PulseView's Segment::begin/continue/end_sample_iteration design.
// The decode thread can use these to batch-read sample data without
// calling get_samples() for every chunk, and the iterator count
// prevents free_data/free_head_blocks from freeing memory mid-read.

std::unique_ptr<LogicSnapshot::SegmentDataIterator>
LogicSnapshot::begin_sample_iteration(uint64_t start, int sig_index) {
  // C3: FINITE (non-loop) fast path is lock-free — iterates the immutable
  // non-loop tree (`_loop_offset`==0). The iterator refcount still guards
  // against post-capture free_unused_memory/free_head_blocks.
  if (!_is_loop) {
    auto it = std::make_unique<SegmentDataIterator>();
    begin_iteration();  // increment _iterator_count
    it->current_sample = start;
    it->ch_order = get_ch_order(sig_index);
    if (it->ch_order < 0 || (unsigned int)it->ch_order >= _ch_data.size()) {
      it->exhausted = true;
      it->chunk_data = nullptr;
      it->chunk_remaining = 0;
      return it;
    }
    it->root_index = start >> (LeafBlockPower + RootScalePower);
    it->lbp_index = (start & RootMask) >> LeafBlockPower;
    it->byte_offset = (start & LeafMask) / 8;
    if (it->root_index < _ch_data[it->ch_order].size()) {
      void *ptr = _ch_data[it->ch_order][it->root_index].lbp[it->lbp_index];
      if (ptr) {
        it->chunk_data = (const uint8_t*)ptr;
        uint64_t leaf_bytes = LeafBlockSamples / 8;
        it->chunk_remaining = leaf_bytes - it->byte_offset;
      } else {
        it->chunk_data = nullptr;
        it->chunk_remaining = 0;
      }
    } else {
      it->exhausted = true;
    }
    return it;
  }

  std::lock_guard<std::recursive_mutex> lock(_mutex);

  auto it = std::make_unique<SegmentDataIterator>();
  begin_iteration();  // increment _iterator_count

  it->current_sample = start + _loop_offset;
  it->ch_order = get_ch_order(sig_index);

  if (it->ch_order < 0 || (unsigned int)it->ch_order >= _ch_data.size()) {
    it->exhausted = true;
    it->chunk_data = nullptr;
    it->chunk_remaining = 0;
    return it;
  }

  // Compute root/lbp/byte indices for the start sample
  it->root_index = it->current_sample >> (LeafBlockPower + RootScalePower);
  it->lbp_index = (it->current_sample & RootMask) >> LeafBlockPower;
  it->byte_offset = (it->current_sample & LeafMask) / 8;

  // Load the first chunk
  if (it->root_index < _ch_data[it->ch_order].size()) {
    void *ptr = _ch_data[it->ch_order][it->root_index].lbp[it->lbp_index];
    if (ptr) {
      it->chunk_data = (const uint8_t*)ptr;
      uint64_t leaf_bytes = LeafBlockSamples / 8;
      it->chunk_remaining = leaf_bytes - it->byte_offset;
    } else {
      // Constant-value block — no actual data pointer
      it->chunk_data = nullptr;
      it->chunk_remaining = 0;
    }
  } else {
    it->exhausted = true;
  }

  return it;
}

void LogicSnapshot::continue_sample_iteration(SegmentDataIterator* it,
                                               uint64_t increase) {
  if (!it || it->exhausted)
    return;

  // C3: FINITE (non-loop) fast path is lock-free; exhaustion is bounded by
  // the release-published committed count.
  if (!_is_loop) {
    it->current_sample += increase;
    uint64_t new_root = it->current_sample >> (LeafBlockPower + RootScalePower);
    uint64_t new_lbp  = (it->current_sample & RootMask) >> LeafBlockPower;
    uint64_t new_off  = (it->current_sample & LeafMask) / 8;
    if (new_root != it->root_index || new_lbp != it->lbp_index) {
      it->root_index = new_root;
      it->lbp_index = new_lbp;
      it->byte_offset = new_off;
      if ((unsigned int)it->ch_order < _ch_data.size() &&
          it->root_index < _ch_data[it->ch_order].size()) {
        void *ptr = _ch_data[it->ch_order][it->root_index].lbp[it->lbp_index];
        if (ptr) {
          it->chunk_data = (const uint8_t*)ptr;
          uint64_t leaf_bytes = LeafBlockSamples / 8;
          it->chunk_remaining = leaf_bytes - it->byte_offset;
        } else {
          it->chunk_data = nullptr;
          it->chunk_remaining = 0;
        }
      } else {
        it->exhausted = true;
        it->chunk_data = nullptr;
        it->chunk_remaining = 0;
      }
    } else {
      uint64_t advance_bytes = new_off - it->byte_offset;
      it->byte_offset = new_off;
      if (advance_bytes >= it->chunk_remaining) {
        it->chunk_remaining = 0;
      } else {
        it->chunk_remaining -= advance_bytes;
      }
    }
    if (it->current_sample >= _ring_published.load(std::memory_order_acquire))
      it->exhausted = true;
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(_mutex);

  it->current_sample += increase;

  // Check if we've moved past the current chunk
  uint64_t new_root = it->current_sample >> (LeafBlockPower + RootScalePower);
  uint64_t new_lbp  = (it->current_sample & RootMask) >> LeafBlockPower;
  uint64_t new_off  = (it->current_sample & LeafMask) / 8;

  if (new_root != it->root_index || new_lbp != it->lbp_index) {
    // Crossed a leaf block boundary — load the new chunk
    it->root_index = new_root;
    it->lbp_index = new_lbp;
    it->byte_offset = new_off;

    if ((unsigned int)it->ch_order < _ch_data.size() &&
        it->root_index < _ch_data[it->ch_order].size()) {
      void *ptr = _ch_data[it->ch_order][it->root_index].lbp[it->lbp_index];
      if (ptr) {
        it->chunk_data = (const uint8_t*)ptr;
        uint64_t leaf_bytes = LeafBlockSamples / 8;
        it->chunk_remaining = leaf_bytes - it->byte_offset;
      } else {
        it->chunk_data = nullptr;
        it->chunk_remaining = 0;
      }
    } else {
      it->exhausted = true;
      it->chunk_data = nullptr;
      it->chunk_remaining = 0;
    }
  } else {
    // Same leaf block — just update offset and remaining
    uint64_t advance_bytes = new_off - it->byte_offset;
    it->byte_offset = new_off;
    if (advance_bytes >= it->chunk_remaining) {
      it->chunk_remaining = 0;
    } else {
      it->chunk_remaining -= advance_bytes;
    }
  }

  // Check if we've passed total sample count
  if (it->current_sample >= _ring_sample_count + _loop_offset) {
    it->exhausted = true;
  }
}

void LogicSnapshot::end_sample_iteration(std::unique_ptr<SegmentDataIterator> it) {
  if (!it)
    return;
  end_iteration();  // decrement _iterator_count
  // unique_ptr destructor deletes the SegmentDataIterator
}

bool LogicSnapshot::get_sample(uint64_t index, int sig_index) {
  // C3 (P9-on-raw): FINITE (non-loop) fast path is lock-free. The committed
  // sample count is acquire-loaded; only fully-captured samples are read, so
  // there is no data race with mipmap metadata mutation or block writes, and
  // (non-loop) no block is ever freed during capture, so the leaf pointer is
  // always valid. `_loop_offset` is 0 for finite captures — not read here.
  if (!_is_loop) {
    const uint64_t N = _ring_published.load(std::memory_order_acquire);
    if (index >= N)
      return false;
    int order = get_ch_order(sig_index);
    if (order == -1 || (unsigned int)order >= _ch_data.size())
      return false;
    uint64_t index0 = index >> (LeafBlockPower + RootScalePower);
    if (index0 >= _ch_data[order].size())
      return false;
    const RootNode &rn = _ch_data[order][index0];
    uint64_t index1 = (index & RootMask) >> LeafBlockPower;
    uint64_t root_pos_mask = 1ULL << index1;
    if ((rn.tog & root_pos_mask) == 0)
      return (rn.first & root_pos_mask) != 0;
    const uint64_t *ptr = (const uint64_t *)rn.lbp[index1];
    if (ptr == nullptr)
      return (rn.first & root_pos_mask) != 0;
    uint64_t u64_idx = (index & LeafMask) >> ScalePower;
    uint64_t index_mask = 1ULL << (index & LevelMask[0]);
    return ptr[u64_idx] & index_mask;
  }

  std::lock_guard<std::recursive_mutex> lock(_mutex);
  return get_sample_unlock(index, sig_index);
}

bool LogicSnapshot::get_sample_unlock(uint64_t index, int sig_index) {
  index += _loop_offset;
  _ring_sample_count += _loop_offset;

  bool flag = get_sample_self(index, sig_index);

  _ring_sample_count -= _loop_offset;
  return flag;
}

bool LogicSnapshot::get_sample_self(uint64_t index, int sig_index) {
  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size()) {
    pxv_warn("LogicSnapshot::get_sample_self: invalid order for sig_index=%d", sig_index);
    return false;
  }

  if (index >= committed_sample_count()) {
    pxv_warn("LogicSnapshot::get_sample_self: index=%llu >= ring=%llu",
             (unsigned long long)index, (unsigned long long)committed_sample_count());
    return false;
  }

  uint64_t index_mask = 1ULL << (index & LevelMask[0]);
  uint64_t index0 = index >> (LeafBlockPower + RootScalePower);
  uint64_t index1 = (index & RootMask) >> LeafBlockPower;
  uint64_t root_pos_mask = 1ULL << index1;

  if (index0 >= _ch_data[order].size()) {
    pxv_warn("LogicSnapshot::get_sample_self: index0=%llu out of range (size=%zu)",
             (unsigned long long)index0, _ch_data[order].size());
    return false;
  }

  if ((_ch_data[order][index0].tog & root_pos_mask) == 0)
    return (_ch_data[order][index0].first & root_pos_mask) != 0;

  void *ptr = _ch_data[order][index0].lbp[index1];
  if (ptr == nullptr)
    return (_ch_data[order][index0].first & root_pos_mask) != 0;

  uint64_t u64_idx = (index & LeafMask) >> ScalePower;
  uint64_t *u64_ptr = (uint64_t *)ptr;
  return u64_ptr[u64_idx] & index_mask;
}

bool LogicSnapshot::get_display_edges(
    std::vector<std::pair<bool, bool>> &edges,
    std::vector<std::pair<uint16_t, bool>> &togs, uint64_t start, uint64_t end,
    uint16_t width, uint16_t max_togs, double pixels_offset, double min_length,
    uint16_t sig_index) {
  return _edge_scan->get_display_edges(edges, togs, start, end, width, max_togs,
                                       pixels_offset, min_length, sig_index);
}


bool LogicSnapshot::get_nxt_edge(uint64_t &index, bool last_sample,
                                 uint64_t end, double min_length,
                                 int sig_index) {
  return _edge_scan->get_nxt_edge(index, last_sample, end, min_length,
                                  sig_index);
}



bool LogicSnapshot::find_first_different_raw(int order, uint64_t start,
                                             uint64_t end, bool expected_level,
                                             uint64_t &out_pos) {
  // P5 diff 扫描: 直接扫 raw 块字节, 不做 mipmap 树遍历.
  // 逐 u64 与 expected_level 求差分 (v ^ (expected ? ~0 : 0)), 用 bsf_folded
  // (ctz) 定位第一个差异位. 未实例化块 (nullptr) 由 first/last 常量值判定.
  // 调用方持有 _mutex (filter 在 apply_glitch_filter 内调用).
  if (start > end)
    return false;
  if ((unsigned int)order >= _ch_data.size())
    return false;

  uint64_t idx0 = start / (LeafBlockSamples * RootScale);
  uint64_t idx1 = (start / LeafBlockSamples) % RootScale;
  uint64_t pos = start;

  while (pos <= end && idx0 < (uint64_t)_ch_data[order].size()) {
    void *lbp = _ch_data[order][idx0].lbp[idx1];
    const uint64_t blk_start = (idx0 * RootScale + idx1) * LeafBlockSamples;
    if (lbp == nullptr) {
      // 常量块 (未实例化): 整块电平 = first bit. start 处电平 == expected,
      // 故 const == expected 时本块无差异, 跳到下一块; 否则返回本块起点 pos.
      // 注意: 后续块 (非首块) 的 pos == blk_start > start, 必须返回 pos 而非
      // start — start 可能在更早块, 其电平仍 == expected (原实现 out_pos=start
      // 会错误回跳, 毛刺滤波把跳变误判到搜索起点).
      const bool const_val =
          (_ch_data[order][idx0].first & (1ULL << idx1)) != 0;
      if (const_val != expected_level) {
        out_pos = pos;
        return true;
      }
      pos = blk_start + LeafBlockSamples;
      if (++idx1 >= RootScale) { idx1 = 0; idx0++; }
      continue;
    }

    const uint64_t *u64s = (const uint64_t *)lbp;
    uint64_t local = pos - blk_start;
    while (local < LeafBlockSamples && pos <= end) {
      const uint64_t u64_idx = local >> ScalePower;   // /64
      const uint64_t bit_in_u64 = local & (Scale - 1); // %64
      const uint64_t v = u64s[u64_idx];
      const uint64_t diff = v ^ (expected_level ? ~0ULL : 0ULL);
      const uint64_t t = diff & (~0ULL << bit_in_u64);
      if (t != 0) {
        const uint64_t found = blk_start + (u64_idx << ScalePower) +
                               bsf_folded(t);
        if (found > end)
          return false;
        out_pos = found;
        return true;
      }
      local = (u64_idx + 1) << ScalePower;  // 跳到下一 u64
      pos = blk_start + local;
    }

    pos = blk_start + LeafBlockSamples;
    if (++idx1 >= RootScale) { idx1 = 0; idx0++; }
  }
  return false;
}

bool LogicSnapshot::get_pre_edge(uint64_t &index, bool last_sample,
                                 double min_length, int sig_index) {
  return _edge_scan->get_pre_edge(index, last_sample, min_length, sig_index);
}


bool LogicSnapshot::pattern_search(int64_t start, int64_t end, int64_t &index,
                                   std::map<uint16_t, QString> &pattern,
                                   bool isNext) {
  return _pattern_search->pattern_search(start, end, index, pattern, isNext);
}

bool LogicSnapshot::has_data(int sig_index) {
  return get_ch_order(sig_index) != -1;
}

int LogicSnapshot::get_block_num() {
  int block =
      ceil((_ring_sample_count + _loop_offset) * 1.0 / LeafBlockSamples) -
      floor(_loop_offset * 1.0 / LeafBlockSamples);
  return block;
}

uint64_t LogicSnapshot::get_block_size(int block_index) {
  int block_num = get_block_num();
  uint64_t samples = 0;

  assert(block_index < block_num);
  if (block_index >= block_num || block_index < 0) {
    pxv_warn("LogicSnapshot::get_block_size: block_index=%d block_num=%d, returning 0",
             block_index, block_num);
    return 0;
  }

  if (_loop_offset > 0) {
    if (block_index > 0 && block_index < block_num - 1) {
      return LeafBlockSamples / 8;
    } else if (block_index == 0) {
      samples =
          min(_ring_sample_count + (_loop_offset % (uint64_t)LeafBlockSamples),
              (uint64_t)LeafBlockSamples) -
          (_loop_offset % (uint64_t)LeafBlockSamples);
      return samples / 8;
    } else {
      samples = (_ring_sample_count + _loop_offset) -
                (_ring_sample_count + _loop_offset - 1) / LeafBlockSamples *
                    LeafBlockSamples;
      return samples / 8;
    }
  } else {
    if (block_index < block_num - 1) {
      return LeafBlockSamples / 8;
    } else {
      if (_ring_sample_count % LeafBlockSamples == 0)
        return LeafBlockSamples / 8;
      else
        return (_ring_sample_count % LeafBlockSamples) / 8;
    }
  }
}

uint8_t *LogicSnapshot::get_block_buf(int block_index, int sig_index,
                                      bool &sample) {
  // Guard: block_index may be out of range during file loading.
  if (block_index < 0 || block_index >= get_block_num()) {
    pxv_warn("LogicSnapshot::get_block_buf: block_index=%d out of range, returning nullptr",
             block_index);
    sample = 0;
    return nullptr;
  }

  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size()) {
    sample = 0;
    return nullptr;
  }

  int block_index0 = block_index;
  block_index += _loop_offset / LeafBlockSamples;

  uint64_t index = block_index / RootScale;
  uint8_t pos = block_index % RootScale;
  if (index >= _ch_data[order].size()) {
    sample = 0;
    return nullptr;
  }
  uint8_t *lbp = (uint8_t *)_ch_data[order][index].lbp[pos];

  if (lbp == nullptr) {
    sample = (_ch_data[order][index].first & 1ULL << pos) != 0;
  }

  if (lbp != nullptr && _loop_offset > 0 && block_index0 == 0) {
    lbp += (_loop_offset % LeafBlockSamples) / 8;
  }

  return lbp;
}

int LogicSnapshot::get_ch_order(int sig_index) {
  uint16_t order = 0;

  for (uint16_t i : _ch_index) {
    if (i == sig_index)
      return order;
    else
      order++;
  }

  return -1;
}

void LogicSnapshot::move_first_node_to_last() {
  for (unsigned int i = 0; i < _channel_num; i++) {
    struct RootNode rn = _ch_data[i][0];
    _ch_data[i].erase(_ch_data[i].begin());

    for (int x = 0; x < (int)Scale; x++) {
      if (rn.lbp[x] != nullptr) {
        push_to_free_list(rn.lbp[x]);
        rn.lbp[x] = nullptr;
      }
    }

    rn.tog = 0;
    rn.first = 0;
    rn.last = 0;

    _ch_data[i].push_back(rn);
  }
}

void LogicSnapshot::decode_end() {
  std::lock_guard<std::recursive_mutex> lock(_mutex);

  std::sort(_free_block_list.begin(), _free_block_list.end());
  _free_block_list.erase(
      std::unique(_free_block_list.begin(), _free_block_list.end()),
      _free_block_list.end());
  for (void *p : _free_block_list) {
    LeafBlockPool::instance().release(p);
  }
  _free_block_list.clear();
}

void LogicSnapshot::push_to_free_list(void* ptr) {
  if (!ptr) return;
  if (_mmap_alloc && _mmap_alloc->is_mmap_address(ptr)) {
    // Decommit physical pages back to OS. If decommit_block() returns true
    // (Linux madvise on anonymous mapping), the OS guarantees zero-fill on
    // next access — clear the written flag so the slot is treated as fresh
    // (skip memset, rely on OS zero-fill). If it returns false (Windows
    // SEC_RESERVE or file-backed), pages may retain content — keep the
    // written flag so the allocator explicitly memsets on reuse.
    const bool os_zeroed = _mmap_alloc->decommit_block(ptr, LeafBlockSpace);
    if (os_zeroed) {
      uint64_t abs_slot = 0;
      if (_mmap_alloc->block_absolute_slot(ptr, LeafBlockSpace, abs_slot)) {
        clear_mmap_slot_by_abs(abs_slot);
      }
    }
    return;
  }
  _free_block_list.push_back(ptr);
}

void LogicSnapshot::free_decode_lpb(void *lbp) {
  if (!lbp) {
    pxv_warn("%s", "LogicSnapshot::free_decode_lpb: lbp is nullptr");
    return;
  }
  assert(lbp);

  std::lock_guard<std::recursive_mutex> lock(_mutex);
  if (_mmap_alloc && _mmap_alloc->is_mmap_address(lbp)) {
      return;
  }

  auto new_end =
      std::remove(_free_block_list.begin(), _free_block_list.end(), lbp);
  if (new_end != _free_block_list.end()) {
    LeafBlockPool::instance().release(lbp);
    _free_block_list.erase(new_end, _free_block_list.end());
  }
}

void LogicSnapshot::free_head_blocks(int count) {
  // P1-6 fix: Skip if there are active iterators to prevent use-after-free.
  if (has_active_iterators()) {
    pxv_info("LogicSnapshot::free_head_blocks: deferred — %d active iterators",
             _iterator_count.load());
    return;
  }

  // Guard: count must be valid. Stale state during file loading
  // could violate these invariants. Original asserts crash via MSVC dialog.
  if (count <= 0 || count >= (int)Scale) {
    pxv_warn("LogicSnapshot::free_head_blocks: count=%d invalid (Scale=%d), skipping",
             count, (int)Scale);
    return;
  }

  for (int i = 0; i < (int)_channel_num; i++) {
    for (int j = _lst_free_block_index; j < count; j++) {
      if (_ch_data[i][0].lbp[j] != nullptr) {
        push_to_free_list(_ch_data[i][0].lbp[j]);
        _ch_data[i][0].lbp[j] = nullptr;
      }

      _ch_data[i][0].tog = (_ch_data[i][0].tog >> count) << count;
      _ch_data[i][0].first = (_ch_data[i][0].first >> count) << count;
      _ch_data[i][0].last = (_ch_data[i][0].last >> count) << count;
    }
  }
  _lst_free_block_index = count;
}

// B-6: Override to connect the PulseView-style free_unused_memory() API
// to the existing free_head_blocks() mechanism. Called after capture_ended()
// (which calls set_complete() → _mem_optimization_requested = true).
// If there are active iterators (decode thread holding get_samples pointers),
// release is deferred — free_head_blocks() also checks this internally.
void LogicSnapshot::free_unused_memory() {
  // Check the base-class flag first; if nobody requested optimization,
  // there's nothing to do. Reset the flag regardless.
  if (!_mem_optimization_requested) {
    return;
  }
  _mem_optimization_requested = false;

  // Skip if the decode thread is actively reading (has active iterators).
  if (has_active_iterators()) {
    pxv_dbg("LogicSnapshot::free_unused_memory: deferred — %d active iterators",
            _iterator_count.load());
    // Re-set the flag so the next call can retry.
    _mem_optimization_requested = true;
    return;
  }

  // In loop mode, _loop_offset tracks how far the ring has wrapped.
  // The wrapped (old) data in root node 0 can be freed.
  if (_loop_offset > 0) {
    int free_count = static_cast<int>(_loop_offset / LeafBlockSamples);
    if (free_count > _lst_free_block_index) {
      // Cap to Scale (root node capacity) to match free_head_blocks assert.
      if (free_count >= (int)Scale)
        free_count = (int)Scale - 1;
      if (free_count > 0) {
        free_head_blocks(free_count);
        pxv_info("LogicSnapshot::free_unused_memory: freed %d head blocks",
                 free_count);
      }
    }
  }
}

int LogicSnapshot::get_block_with_sample(uint64_t index, uint64_t *out_offset) {
  if (!out_offset) {
    pxv_warn("%s", "LogicSnapshot::get_block_with_sample: out_offset is nullptr");
    return -1;
  }
  assert(out_offset);

  int block = index / LeafBlockSamples;
  *out_offset = index % LeafBlockSamples;
  return block;
}

void LogicSnapshot::invert_channel(int sig_index) {
  _glitch_filter->invert_channel(sig_index);
}

void LogicSnapshot::apply_glitch_filter(
    int sig_index, uint32_t threshold,
    std::function<void(int)> progress_callback,
    GlitchFilterMode filter_mode) {
  _glitch_filter->apply_glitch_filter(sig_index, threshold,
                                      std::move(progress_callback), filter_mode);
}

void LogicSnapshot::apply_glitch_filter_all(
    const std::map<int, uint32_t> &thresholds,
    std::function<void(int)> progress_callback,
    const std::map<int, GlitchFilterMode> &filter_modes) {
  _glitch_filter->apply_glitch_filter_all(thresholds,
                                          std::move(progress_callback),
                                          filter_modes);
}

bool LogicSnapshot::is_glitch_filtered() {
  return _glitch_filter->is_glitch_filtered();
}

void LogicSnapshot::set_glitch_filtered(bool filtered) {
  _glitch_filter->set_glitch_filtered(filtered);
}

const std::vector<LogicSnapshot::FillRange>&
LogicSnapshot::get_filtered_ranges(int sig_index) const {
  return _glitch_filter->get_filtered_ranges(sig_index);
}

void LogicSnapshot::clear_filtered_ranges() {
  _glitch_filter->clear_filtered_ranges();
}

} // namespace data
} // namespace pv
