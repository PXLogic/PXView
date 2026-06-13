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
#include <assert.h>
#include <functional>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include "../dsvdef.h"
#include "../log.h"
#include "../utility/array.h"
#include "leaf_block_pool.h"
#include "logicsnapshot.h"
#include <map>

using namespace std;

namespace pv {
namespace data {

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
  _glitch_filtered = false;
  _mmap_alloc = nullptr;
  _max_blocks_per_channel = 0;
  _async_running = false;
  _async_bytes_written = 0;
  _async_write_speed_mbps = 0.0;
  _async_queue_depth = 0;
  _async_queue_bytes_size = 0;
}
LogicSnapshot::~LogicSnapshot() {
  if (_async_running) {
    _async_running = false;
    _async_cv.notify_one();
    if (_async_thread.joinable()) {
      _async_thread.join();
    }
  }
}
void LogicSnapshot::free_data() {
  _mmap_alloc.reset();

  Snapshot::free_data();

  // With mmap, all data is in the mmapped file/memory.
  // We don't need to release individual LeafBlocks back to LeafBlockPool,
  // except for those that were manually allocated (e.g., if fallback happened, but ideally none).
  for (auto &iter : _ch_data) {
    std::vector<struct RootNode> void_vector;
    iter.swap(void_vector);
  }
  _ch_data.clear();
  _sample_count = 0;

  for (void *p : _free_block_list) {
    LeafBlockPool::instance().release(p);
  }
  _free_block_list.clear();
}

void LogicSnapshot::init() {
  std::lock_guard<std::mutex> lock(_mutex);
  init_all();
}

void LogicSnapshot::init_all() {
  _sample_count = 0;
  _ring_sample_count = 0;
  _byte_fraction = 0;
  _ch_fraction = 0;
  _dest_ptr = NULL;
  _memory_failed = false;
  _last_ended = true;
  _loop_offset = 0;
  _able_free = true;
}

void LogicSnapshot::clear() {
  if (_async_running) {
    _async_running = false;
    _async_cv.notify_one();
    if (_async_thread.joinable()) {
      _async_thread.join();
    }
  }
  {
    std::lock_guard<std::mutex> lock(_async_mutex);
    while (!_async_queue.empty()) _async_queue.pop();
    _async_queue_depth = 0;
    _async_queue_bytes_size = 0;
  }

  std::lock_guard<std::mutex> lock(_mutex);
  free_data();
  init_all();
}

void LogicSnapshot::set_disk_cache_config(const DiskCacheConfig &config) {
  pxv_info("LogicSnapshot::set_disk_cache_config: enabled=%d, path=%s",
           config.enabled, config.cache_path.c_str());
  _disk_cache_config = config;
}

bool LogicSnapshot::is_disk_cache_active() {
  return _mmap_alloc && _disk_cache_config.enabled;
}

double LogicSnapshot::get_disk_write_speed_mbps() {
  return _async_write_speed_mbps.load();
}

size_t LogicSnapshot::get_disk_write_queue_depth() {
  return _async_queue_depth.load();
}

uint64_t LogicSnapshot::get_disk_total_blocks_written() {
  return _async_bytes_written.load() / LeafBlockSpace;
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
  return _async_queue_bytes_size.load();
}

void LogicSnapshot::ensure_all_blocks_hot() {
}

void LogicSnapshot::first_payload(const sr_datafeed_logic &logic,
                                  uint64_t total_sample_count, GSList *channels,
                                  bool able_free) {
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

  std::unique_lock<std::mutex> lock(_mutex);

  if (total_sample_count != _total_sample_count ||
      channel_num != _channel_num || channel_changed || _is_loop) {

    free_data();
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
      pxv_info("ERROR: all channels disalbed");
      assert(0);
    }
  } else {
    for (auto &iter : _ch_data) {
      for (auto &iter_rn : iter) {
        iter_rn.tog = 0;
        iter_rn.first = 0;
        iter_rn.last = 0;

        for (int j = 0; j < 64; j++) {
          if (iter_rn.lbp[j] != NULL)
            memset(iter_rn.lbp[j], 0, LeafBlockSpace);
        }
      }
    }
  }

  assert(_channel_num < CHANNEL_MAX_COUNT);

  _sample_count = 0;
  _ring_sample_count = 0;

  for (unsigned int i = 0; i < _channel_num; i++) {
    _last_sample[i] = 0;
    _last_calc_count[i] = 0;
    _cur_ref_block_indexs[i].root_index = 0;
    _cur_ref_block_indexs[i].lbp_index = 0;
  }

  pxv_info("LogicSnapshot::first_payload: disk_cache_config.enabled=%d, "
           "ch_data.size()=%zu",
           _disk_cache_config.enabled, _ch_data.size());

  if (_channel_num > 0) {
    // Create and configure MmapAllocator
    _mmap_alloc = std::make_shared<MmapAllocator>();
    
    // Calculate total required memory based on total_sample_count + padding
    // For loop mode, _total_sample_count is the size of the ring buffer.
    _max_blocks_per_channel = (_total_sample_count / LeafBlockSamples) + 16; 
    uint64_t total_bytes = _max_blocks_per_channel * LeafBlockSpace * _channel_num;
    
    bool use_disk = _disk_cache_config.enabled;
    QString disk_dir = QString::fromStdString(_disk_cache_config.cache_path);
    if (!_mmap_alloc->configure(use_disk, disk_dir, total_bytes)) {
        pxv_err("LogicSnapshot::first_payload: MmapAllocator configure failed!");
    }
  }

  if (!_async_running) {
    _async_running = true;
    _async_bytes_written = 0;
    _async_thread = std::thread(&LogicSnapshot::async_write_worker, this);
  }

  lock.unlock();
  append_payload(logic);
  _last_ended = false;
}

void LogicSnapshot::append_payload(const sr_datafeed_logic &logic) {
  if (logic.length == 0 || logic.data == nullptr) return;
  
  AsyncPayload payload;
  payload.format = logic.format;
  payload.data = std::vector<uint8_t>((uint8_t*)logic.data, (uint8_t*)logic.data + logic.length);
  size_t v_size = payload.data.size();
  
  {
    std::lock_guard<std::mutex> lock(_async_mutex);
    _async_queue.push(std::move(payload));
    _async_queue_depth = _async_queue.size();
    _async_queue_bytes_size += v_size;
  }
  _async_cv.notify_one();
}

void LogicSnapshot::async_write_worker() {
  while (_async_running) {
    AsyncPayload payload;
    {
      std::unique_lock<std::mutex> lock(_async_mutex);
      _async_cv.wait_for(lock, std::chrono::milliseconds(10), [this] {
        return !_async_running || !_async_queue.empty();
      });
      
      if (!_async_running && _async_queue.empty()) break;
      if (_async_queue.empty()) continue;
      
      payload = std::move(_async_queue.front());
      _async_queue.pop();
      _async_queue_depth = _async_queue.size();
      _async_queue_bytes_size -= payload.data.size();
    }
    
    sr_datafeed_logic logic;
    logic.length = payload.data.size();
    logic.unitsize = 1; // Assuming unitsize 1
    logic.data = payload.data.data();
    logic.format = payload.format;
    
    // Truncate incomplete chunks to prevent channel desynchronization
    if (logic.format == LA_CROSS_DATA) {
        uint64_t chunk_size = _channel_num * 8; // Each channel gets 8 bytes (64 samples) per chunk
        logic.length -= logic.length % chunk_size; // Truncate to exact multiple
    }
    
    static int packet_count = 0;
    if (packet_count < 5 || logic.length % 128 != 0 || packet_count % 100 == 0) {
        pxv_info("async_write_worker: pkt %d, len=%llu, first_bytes: %02x %02x %02x %02x", 
                 packet_count, (unsigned long long)logic.length,
                 logic.length > 0 ? ((uint8_t*)logic.data)[0] : 0,
                 logic.length > 1 ? ((uint8_t*)logic.data)[1] : 0,
                 logic.length > 2 ? ((uint8_t*)logic.data)[2] : 0,
                 logic.length > 3 ? ((uint8_t*)logic.data)[3] : 0);
    }
    packet_count++;
    
    auto start = std::chrono::steady_clock::now();
    
    {
      std::lock_guard<std::mutex> lock(_mutex);
      append_cross_payload(logic);
    }
    
    auto end = std::chrono::steady_clock::now();
    _async_bytes_written += payload.data.size();
    
    double elapsed_s = std::chrono::duration<double>(end - start).count();
    if (elapsed_s > 0) {
       double mbps = (payload.data.size() / (1024.0 * 1024.0)) / elapsed_s;
       // Exponential moving average for smoothing UI
       double old = _async_write_speed_mbps.load();
       if (old == 0.0) _async_write_speed_mbps = mbps;
       else _async_write_speed_mbps = old * 0.8 + mbps * 0.2;
    }
  }
}

void* LogicSnapshot::allocate_block(uint16_t channel, uint64_t index0, uint64_t index1) {
    void* lbp = _ch_data[channel][index0].lbp[index1];
    if (lbp != NULL) return lbp;

    if (_mmap_alloc) {
        uint64_t global_block_seq = index0 * RootScale + index1;
        lbp = _mmap_alloc->get_block_data(channel, global_block_seq, _max_blocks_per_channel, LeafBlockSpace);
    }
    if (lbp == NULL) {
        lbp = LeafBlockPool::instance().acquire(LeafBlockSpace);
        if (lbp == NULL) {
            pxv_err("LogicSnapshot: Malloc memory failed!");
            return NULL;
        }
    }
    _ch_data[channel][index0].lbp[index1] = lbp;
    memset(lbp, 0, LeafBlockSpace);
    return lbp;
}

void LogicSnapshot::append_cross_payload(const sr_datafeed_logic &logic) {
  assert(logic.format == LA_CROSS_DATA);
  assert(logic.length >= ScaleSize * _channel_num);
  assert(logic.data);

  if (logic.length % 128 != 0) {
      pxv_warn("append_cross_payload: length %llu is NOT a multiple of 128!", (unsigned long long)logic.length);
  }

  uint8_t *data_src_ptr = (uint8_t *)logic.data;
  uint64_t len = logic.length;
  uint64_t index0 = 0;
  uint64_t index1 = 0;
  uint64_t offset = 0;
  void *lbp = NULL;

  // samples not accurate, lead to a larger _sampole_count
  // _sample_count should be fixed in the last packet
  // so _total_sample_count must be align to LeafBlock
  uint64_t samples = ceil(logic.length * 8.0 / _channel_num);

  if (_sample_count + samples < _total_sample_count) {
    _sample_count += samples;
  } else {
    if (_sample_count == _total_sample_count && !_is_loop)
      return;
    _sample_count = _total_sample_count;
  }

  // pxv_info("_loop_offset:%llu, _total_sample_count:%llu,
  // _ring_sample_count:%llu, cur samples:%llu",
  //     _loop_offset, _total_sample_count, _ring_sample_count, samples);

  if (_is_loop) {
    if (_loop_offset >= LeafBlockSamples * Scale) {
      move_first_node_to_last();
      _loop_offset -= LeafBlockSamples * Scale;
      _lst_free_block_index = 0;
    } else {
      int free_count = _loop_offset / LeafBlockSamples;
      if (free_count > _lst_free_block_index) {
        free_head_blocks(free_count);
      }
    }
  }

  _ring_sample_count += _loop_offset;

  // bit align
  while ((_ch_fraction != 0 || _byte_fraction != 0) && len > 0) {
    if (_dest_ptr == NULL)
      assert(false);

    do {
      *_dest_ptr++ = *data_src_ptr++;
      _byte_fraction = (_byte_fraction + 1) % 8;
      len--;
    } while (_byte_fraction != 0 && len > 0);

    if (_byte_fraction == 0) {
      index0 = _ring_sample_count / LeafBlockSamples / RootScale;
      index1 = (_ring_sample_count / LeafBlockSamples) % RootScale;
      offset = (_ring_sample_count % LeafBlockSamples) / 8;

      _ch_fraction = (_ch_fraction + 1) % _channel_num;

      lbp = allocate_block(_ch_fraction, index0, index1);
      if (lbp == NULL) return;

      _dest_ptr = (uint8_t *)lbp + offset;

      // To the last channel.
      if (_ch_fraction == 0) {
        _ring_sample_count += Scale;

        if (_ring_sample_count % LeafBlockSamples == 0) {
          calc_mipmap(_channel_num - 1, index0, index1, LeafBlockSamples, true);
        }
        break;
      }
    }
  }

  // append data
  assert(_ch_fraction == 0);
  assert(_byte_fraction == 0);
  assert(_ring_sample_count % Scale == 0);

  uint64_t align_sample_count = _ring_sample_count;
  uint64_t *read_ptr = (uint64_t *)data_src_ptr;
  void *end_read_ptr = (uint8_t *)data_src_ptr + len;

  uint64_t filled_sample = align_sample_count % LeafBlockSamples;
  uint64_t old_filled_sample = filled_sample;
  uint64_t *chans_read_addr[CHANNEL_MAX_COUNT];

  for (unsigned int i = 0; i < _channel_num; i++) {
    chans_read_addr[i] = (uint64_t *)data_src_ptr + i;
  }

  uint16_t fill_chan = _ch_fraction;
  uint16_t last_chan = _ch_fraction;
  index0 = align_sample_count / LeafBlockSamples / RootScale;
  index1 = (align_sample_count / LeafBlockSamples) % RootScale;
  offset = align_sample_count % LeafBlockSamples;

  if (index0 >= _ch_data[0].size()) {
    assert(false);
  }

  lbp = allocate_block(fill_chan, index0, index1);
  if (lbp == NULL) return;

  uint64_t *write_ptr = (uint64_t *)lbp + offset / Scale;

  while (len >= 8) {
    *write_ptr++ = *read_ptr;
    read_ptr += _channel_num;
    len -= 8;
    filled_sample += Scale;

    last_chan++;
    if (last_chan == _channel_num) {
      last_chan = 0;
    }

    if (filled_sample == LeafBlockSamples) {
      calc_mipmap(fill_chan, index0, index1, LeafBlockSamples, true);

      chans_read_addr[fill_chan] = read_ptr;
      fill_chan = (fill_chan + 1) % _channel_num;

      if (fill_chan == 0)
        align_sample_count += (filled_sample - old_filled_sample);

      index0 = align_sample_count / LeafBlockSamples / RootScale;
      index1 = (align_sample_count / LeafBlockSamples) % RootScale;
      offset = align_sample_count % LeafBlockSamples;
      filled_sample = align_sample_count % LeafBlockSamples;
      old_filled_sample = filled_sample;

      lbp = allocate_block(fill_chan, index0, index1);
      if (lbp == NULL) return;

      write_ptr = (uint64_t *)lbp + offset / Scale;
      read_ptr = chans_read_addr[fill_chan];
    } else if (read_ptr >= end_read_ptr) {
      calc_mipmap(fill_chan, index0, index1, filled_sample, false);

      fill_chan = (fill_chan + 1) % _channel_num;

      if (fill_chan == 0)
        align_sample_count += (filled_sample - old_filled_sample);

      index0 = align_sample_count / LeafBlockSamples / RootScale;
      index1 = (align_sample_count / LeafBlockSamples) % RootScale;
      offset = align_sample_count % LeafBlockSamples;
      filled_sample = align_sample_count % LeafBlockSamples;
      old_filled_sample = filled_sample;

      lbp = allocate_block(fill_chan, index0, index1);
      if (lbp == NULL) return;

      write_ptr = (uint64_t *)lbp + offset / Scale;
      read_ptr = chans_read_addr[fill_chan];
    }
  }

  _ring_sample_count = align_sample_count;
  _ring_sample_count -= _loop_offset;

  if (align_sample_count > _total_sample_count) {
    _loop_offset = align_sample_count - _total_sample_count;
    _ring_sample_count = _total_sample_count;
  }

  _ch_fraction = last_chan;

  lbp = allocate_block(_ch_fraction, index0, index1);
  if (lbp == NULL) return;

  _dest_ptr = (uint8_t *)lbp + offset / 8;

  if (len > 0) {
    uint8_t *src_ptr = (uint8_t *)end_read_ptr - len;
    _byte_fraction += len;

    while (len > 0) {
      *_dest_ptr++ = *src_ptr++;
      len--;
    }
  }
}

void LogicSnapshot::capture_ended() {
  // CRITICAL FIX: Drain the async write queue BEFORE acquiring _mutex.
  // Without this, _ring_sample_count may be stale (the async writer hasn't
  // finished writing all pending data), and the memset below would zero out
  // valid data that was still waiting in the queue.
  // We must NOT hold _mutex while waiting, because the async worker needs
  // _mutex to call append_cross_payload().
  {
    int drain_loops = 0;
    while (true) {
      {
        std::lock_guard<std::mutex> lock(_async_mutex);
        if (_async_queue.empty()) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      drain_loops++;
      if (drain_loops > 10000) { // 10s safety timeout
        pxv_err("capture_ended: async queue drain timeout!");
        break;
      }
    }
    if (drain_loops > 0) {
      pxv_info("capture_ended: drained async queue in %d ms", drain_loops);
    }
  }

  std::lock_guard<std::mutex> lock(_mutex);

  Snapshot::capture_ended();

  _sample_count = _ring_sample_count;
  _ring_sample_count += _loop_offset;

  uint64_t index0 = _ring_sample_count / LeafBlockSamples / RootScale;
  uint64_t index1 = (_ring_sample_count / LeafBlockSamples) % RootScale;
  uint64_t offset = (_ring_sample_count % LeafBlockSamples) / 8;

  _ring_sample_count -= _loop_offset;

  if (offset > 0) {
    for (unsigned int chan = 0; chan < _channel_num; chan++) {
      uint8_t *lbp = (uint8_t *)_ch_data[chan][index0].lbp[index1];

      if (lbp == NULL) {
        pxv_err("ERROR:LogicSnapshot::capture_ended(),buffer is null.");
        assert(false);
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
  std::lock_guard<std::mutex> lock(_mutex);

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
  _memory_failed = src._memory_failed;
  _last_ended = src._last_ended;
  _samplerate = src._samplerate;
  _ch_index = src._ch_index;

  _byte_fraction = src._byte_fraction;
  _ch_fraction = src._ch_fraction;
  _dest_ptr = NULL;
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
      _mmap_alloc->configure(false, "", src._mmap_alloc->get_total_bytes());
  } else {
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
        if (rn.lbp[k] != NULL) {
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
          new_rn.lbp[k] = NULL;
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

  if (lbp == NULL)
    return;
  void *level1_ptr = (uint8_t *)lbp + LeafBlockSamples / 8;
  void *level2_ptr = (uint8_t *)level1_ptr + LeafBlockSamples / Scale / 8;
  void *level3_ptr =
      (uint8_t *)level2_ptr + LeafBlockSamples / Scale / Scale / 8;

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

  for (; i < samples / Scale; i++) {
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

  if ((*((uint64_t *)lbp + LeafBlockSamples / Scale - 1) & MSB) != 0)
    _ch_data[order][index0].last |= 1ULL << index1;

  if (*((uint64_t *)level3_ptr) != 0) {
    _ch_data[order][index0].tog |= 1ULL << index1;
  } else if (isEnd) {
    push_to_free_list(_ch_data[order][index0].lbp[index1]);

    _ch_data[order][index0].lbp[index1] = NULL;
  }

  if (isEnd)
    _last_calc_count[order] = 0;
  else
    _last_calc_count[order] = samples;
}

const uint8_t *LogicSnapshot::get_samples(uint64_t start_sample,
                                          uint64_t &end_sample, int sig_index,
                                          void **lbp) {
  std::lock_guard<std::mutex> lock(_mutex);

  uint64_t sample_count = _ring_sample_count;

  assert(start_sample < sample_count);

  if (end_sample >= sample_count)
    end_sample = sample_count - 1;

  assert(end_sample <= sample_count);
  assert(start_sample <= end_sample);

  start_sample += _loop_offset;
  _ring_sample_count += _loop_offset;

  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size())
    return NULL;

  uint64_t index0 = start_sample >> (LeafBlockPower + RootScalePower);
  uint64_t index1 = (start_sample & RootMask) >> LeafBlockPower;
  uint64_t offset = (start_sample & LeafMask) / 8;

  end_sample = (index0 << (LeafBlockPower + RootScalePower)) +
               (index1 << LeafBlockPower) + ~(~0ULL << LeafBlockPower);

  end_sample = min(end_sample + 1, sample_count);

  _ring_sample_count -= _loop_offset;

  if (index0 >= _ch_data[order].size())
    return NULL;

  void *ptr = _ch_data[order][index0].lbp[index1];

  if (ptr == NULL)
    return NULL;

  if (lbp != NULL)
    *lbp = ptr;

  _cur_ref_block_indexs[order].root_index = index0;
  _cur_ref_block_indexs[order].lbp_index = index1;

  return (uint8_t *)ptr + offset;
}

bool LogicSnapshot::get_sample(uint64_t index, int sig_index) {
  std::lock_guard<std::mutex> lock(_mutex);
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
  if (order == -1 || (unsigned int)order >= _ch_data.size())
    return false;

  if (index < _ring_sample_count) {
    uint64_t index_mask = 1ULL << (index & LevelMask[0]);
    uint64_t index0 = index >> (LeafBlockPower + RootScalePower);
    uint64_t index1 = (index & RootMask) >> LeafBlockPower;
    uint64_t root_pos_mask = 1ULL << index1;

    if (index0 >= _ch_data[order].size())
      return false;

    if ((_ch_data[order][index0].tog & root_pos_mask) == 0) {
      return (_ch_data[order][index0].first & root_pos_mask) != 0;
    } else {
      void *ptr = _ch_data[order][index0].lbp[index1];
      if (ptr == NULL)
        return (_ch_data[order][index0].first & root_pos_mask) != 0;
      return *((uint64_t *)ptr + ((index & LeafMask) >> ScalePower)) & index_mask;
    }
  }

  return false;
}

bool LogicSnapshot::get_display_edges(
    std::vector<std::pair<bool, bool>> &edges,
    std::vector<std::pair<uint16_t, bool>> &togs, uint64_t start, uint64_t end,
    uint16_t width, uint16_t max_togs, double pixels_offset, double min_length,
    uint16_t sig_index) {
  if (!edges.empty())
    edges.clear();
  if (!togs.empty())
    togs.clear();

  std::lock_guard<std::mutex> lock(_mutex);

  if (_ring_sample_count == 0)
    return false;

  assert(end < _ring_sample_count);
  assert(start <= end);
  assert(min_length > 0);

  uint64_t index = start;
  bool last_sample;
  bool start_sample;

  // Get the initial state
  start_sample = last_sample = get_sample_unlock(index++, sig_index);
  togs.push_back(pair<uint16_t, bool>(0, last_sample));

  while (edges.size() < width) {
    // search next edge
    bool has_edge = get_nxt_edge_unlock(index, last_sample, end, 0, sig_index);

    // calc the edge position
    int64_t gap = (index / min_length) - pixels_offset;
    index = max((uint64_t)ceil((floor(index / min_length) + 1) * min_length),
                index + 1);

    while (gap > (int64_t)edges.size() && edges.size() < width) {
      edges.push_back(pair<bool, bool>(false, last_sample));
    }

    if (index > end)
      last_sample = get_sample_unlock(end, sig_index);
    else
      last_sample = get_sample_unlock(index - 1, sig_index);

    if (has_edge) {
      edges.push_back(pair<bool, bool>(true, last_sample));
      if (togs.size() < max_togs)
        togs.push_back(pair<uint16_t, bool>(edges.size() - 1, last_sample));
    }

    while (index > end && edges.size() < width)
      edges.push_back(pair<bool, bool>(false, last_sample));
  }

  if (togs.size() < max_togs) {
    last_sample = get_sample_unlock(end, sig_index);
    togs.push_back(pair<uint16_t, bool>(edges.size() - 1, last_sample));
  }

  return start_sample;
}

bool LogicSnapshot::get_nxt_edge(uint64_t &index, bool last_sample,
                                 uint64_t end, double min_length,
                                 int sig_index) {
  std::lock_guard<std::mutex> lock(_mutex);
  return get_nxt_edge_unlock(index, last_sample, end, min_length, sig_index);
}

bool LogicSnapshot::get_nxt_edge_unlock(uint64_t &index, bool last_sample,
                                        uint64_t end, double min_length,
                                        int sig_index) {
  index += _loop_offset;
  end += _loop_offset;
  _ring_sample_count += _loop_offset;

  bool flag = get_nxt_edge_self(index, last_sample, end, min_length, sig_index);

  index -= _loop_offset;
  _ring_sample_count -= _loop_offset;

  return flag;
}

bool LogicSnapshot::get_nxt_edge_self(uint64_t &index, bool last_sample,
                                      uint64_t end, double min_length,
                                      int sig_index) {
  if (index > end)
    return false;

  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size())
    return false;

  // const unsigned int min_level = max((int)floorf(logf(min_length) /
  // logf(Scale)) - 1, 0);
  const unsigned int min_level =
      max((int)(log2f(min_length) - 1) / (int)ScalePower, 0);
  uint64_t root_index = index >> (LeafBlockPower + RootScalePower);
  uint8_t root_pos = (index & RootMask) >> LeafBlockPower;
  bool root_last = (root_index != 0 && root_index - 1 < _ch_data[order].size())
                       ? _ch_data[order][root_index - 1].last & MSB
                       : _ch_data[order][0].first & LSB;
  bool edge_hit = false;

  // linear search for the next transition on the root level
  for (uint64_t i = root_index;
       !edge_hit && (index <= end) && i < (uint64_t)_ch_data[order].size();
       i++) {
    uint64_t cur_mask = (~0ULL << root_pos);

    do {
      uint64_t inner_tog = _ch_data[order][i].tog & cur_mask;
      uint64_t lbp_tog =
          (((_ch_data[order][i].last << 1) + root_last) & cur_mask) ^
          (_ch_data[order][i].first & cur_mask);
      uint8_t inner_tog_pos = bsf_folded(inner_tog);
      uint8_t lbp_tog_pos = bsf_folded(lbp_tog);

      if (inner_tog != 0) {
        if (lbp_tog != 0) {
          // lbp tog before inner tog
          edge_hit = lbp_nxt_edge(index, i, lbp_tog, lbp_tog_pos, true,
                                  inner_tog_pos, last_sample, sig_index);
        }

        if (!edge_hit) {
          void *ptr = _ch_data[order][i].lbp[inner_tog_pos];
          uint64_t blk_start = (i << (LeafBlockPower + RootScalePower)) +
                               (inner_tog_pos << LeafBlockPower);
          index = max(blk_start, index);

          if (ptr != NULL && min_level < ScaleLevel) {
            uint64_t block_end = min(index | LeafMask, end);
            edge_hit =
                block_nxt_edge((uint64_t *)ptr, index, block_end, last_sample, min_level);
          } else if (ptr != NULL) {
            edge_hit = true;
          } else {
            edge_hit = true; // block unavailable, treat as edge
          }

          if (inner_tog_pos == RootScale - 1)
            break;
          cur_mask = (~0ULL << (inner_tog_pos + 1));
        }
      } else if (lbp_tog != 0) {
        // lbp tog
        edge_hit = lbp_nxt_edge(index, i, lbp_tog, lbp_tog_pos, false,
                                Scale - 1, last_sample, sig_index);
      } else {
        // index = (index + (1 << (LeafBlockPower + RootScalePower))) &
        //         (~0ULL << (LeafBlockPower + RootScalePower));
        index = (((i + 1) << (LeafBlockPower + RootScalePower)) - 1);
        break;
      }
    }
    // while (!edge_hit && index < end);
    while (!edge_hit &&
           index < (((i + 1) << (LeafBlockPower + RootScalePower)) - 1));

    root_pos = 0;
    root_last = _ch_data[order][i].last & MSB;
  }

  if (index > end) {
    // skip edges over right
    edge_hit = false;
  }

  // DEBUG: verify edge position
  if (edge_hit && sig_index == 14) {
    bool sample_at_edge = get_sample_self(index, sig_index);
    bool sample_before =
        (index > 0) ? get_sample_self(index - 1, sig_index) : last_sample;
    if (sample_at_edge == sample_before) {
      pxv_warn("[GlitchFilter] FAKE EDGE at %llu: before=%d at=%d (same!)",
               (unsigned long long)index, sample_before, sample_at_edge);
    }
  }

  return edge_hit;
}

bool LogicSnapshot::get_pre_edge(uint64_t &index, bool last_sample,
                                 double min_length, int sig_index) {
  std::lock_guard<std::mutex> lock(_mutex);

  index += _loop_offset;
  _ring_sample_count += _loop_offset;

  bool flag = get_pre_edge_self(index, last_sample, min_length, sig_index);

  index = (index < _loop_offset) ? 0 : index - _loop_offset;
  _ring_sample_count -= _loop_offset;
  return flag;
}

bool LogicSnapshot::get_pre_edge_self(uint64_t &index, bool last_sample,
                                      double min_length, int sig_index) {
  assert(index < _ring_sample_count);

  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size())
    return false;

  // const unsigned int min_level = max((int)floorf(logf(min_length) /
  // logf(Scale)) - 1, 1);
  const unsigned int min_level =
      max((int)(log2f(min_length) - 1) / (int)ScalePower, 0);
  int root_index = index >> (LeafBlockPower + RootScalePower);
  uint8_t root_pos = (index & RootMask) >> LeafBlockPower;
  if ((unsigned int)root_index >= _ch_data[order].size())
    return false;
  bool root_first = _ch_data[order][root_index].last & MSB;
  bool edge_hit = false;

  // linear search for the previous transition on the root level
  for (int64_t i = root_index; !edge_hit && i >= 0; i--) {
    uint64_t cur_mask = (~0ULL >> (RootScale - root_pos - 1));

    do {
      uint64_t inner_tog = _ch_data[order][i].tog & cur_mask;
      uint64_t lbp_tog = (_ch_data[order][i].last & cur_mask) ^
                         ((((uint64_t)root_first << (RootScale - 1)) +
                           (_ch_data[order][i].first >> 1)) &
                          cur_mask);
      uint8_t inner_tog_pos = bsr64(inner_tog);
      uint8_t lbp_tog_pos = bsr64(lbp_tog);

      if (inner_tog != 0) {
        if (lbp_tog != 0) {
          // lbp tog before inner tog
          edge_hit = lbp_pre_edge(index, i, lbp_tog, lbp_tog_pos, true,
                                  inner_tog_pos, last_sample, sig_index);
        }

        if (!edge_hit) {
          void *ptr = _ch_data[order][i].lbp[inner_tog_pos];
          uint64_t blk_end = ((i << (LeafBlockPower + RootScalePower)) +
                              (inner_tog_pos << LeafBlockPower)) |
                             LeafMask;
          index = min(blk_end, index);
          if (ptr != NULL && min_level < ScaleLevel) {
            edge_hit =
                block_pre_edge((uint64_t *)ptr, index, last_sample, min_level, sig_index);
          } else {
            edge_hit = true;
          }
          if (inner_tog_pos == 0)
            break;
          cur_mask = (~0ULL >> (RootScale - inner_tog_pos));
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

    root_pos = RootScale - 1;
    root_first = _ch_data[order][i].first & LSB;
  }

  return edge_hit;
}

bool LogicSnapshot::lbp_nxt_edge(uint64_t &index, uint64_t root_index,
                                 uint64_t lbp_tog, uint8_t lbp_tog_pos,
                                 bool aft_tog, uint8_t aft_pos,
                                 bool last_sample, int sig_index) {
  assert(lbp_tog != 0);

  // check last_sample with current index
  bool sample = get_sample_self(index, sig_index);
  if (sample ^ last_sample) {
    return true;
  }

  // find edge between lbp
  bool edge_hit = false;
  uint64_t aft_lbp_start = (root_index << (LeafBlockPower + RootScalePower)) +
                           (aft_pos << LeafBlockPower);

  while (lbp_tog_pos <= aft_pos) {
    uint64_t lbp_tog_index = (root_index << (LeafBlockPower + RootScalePower)) +
                             (lbp_tog_pos << LeafBlockPower);
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
    if ((lbp_tog_pos < Scale) && (lbp_tog != 0)) {
      lbp_tog_pos = bsf_folded(lbp_tog);
    } else {
      break;
    }
  }

  uint64_t lbp_edge_index =
      aft_tog ? aft_lbp_start : aft_lbp_start + (1ULL << LeafBlockPower) - 1;
  if (!edge_hit && lbp_edge_index > index) {
    index = lbp_edge_index;
  }

  return edge_hit;
}

bool LogicSnapshot::block_nxt_edge(uint64_t *lbp, uint64_t &index,
                                   uint64_t block_end, bool last_sample,
                                   unsigned int min_level) {
  unsigned int level = min_level;
  bool fast_forward = true;
  const uint64_t last = last_sample ? ~0ULL : 0ULL;

  //----- Search Next Edge Within Current LeafBlock -----//
  if (level == 0) {
    // Search individual samples up to the beginning of
    // the next first level mip map block
    const uint64_t offset = (index & ~(~0ULL << LeafBlockPower)) >> ScalePower;
    const uint64_t mask = last_sample ? ~(~0ULL << (index & LevelMask[0]))
                                      : ~0ULL << (index & LevelMask[0]);
    uint64_t sample =
        last_sample ? *(lbp + offset) | mask : *(lbp + offset) & mask;
    if (sample ^ last) {
      index =
          (index & ~LevelMask[0]) + bsf_folded(last_sample ? ~sample : sample);
      fast_forward = false;
    } else {
      index = ((index >> ScalePower) + 1) << ScalePower;
    }
  } else {
    index = ((index >> level * ScalePower) + 1) << level * ScalePower;
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
      const int level_scale_power = (level + 1) * ScalePower;
      const uint64_t offset =
          (index & ~(~0ULL << LeafBlockPower)) >> level_scale_power;
      const uint64_t mask =
          ~0ULL << ((index & LevelMask[level]) >> (level * ScalePower));
      uint64_t sample = *(lbp + LevelOffset[level] + offset) & mask;

      // Check if there was a change in this block
      if (sample) {
        index = (index & (~0ULL << (level + 1) * ScalePower)) +
                (bsf_folded(sample) << level * ScalePower);
        break;
      } else {
        index = ((index >> (level + 1) * ScalePower) + 1)
                << (level + 1) * ScalePower;
        ++level;
      }
    }

    // Zoom in until we encounter a change,
    // and repeat until we reach min_level
    while ((index <= block_end) && (level > min_level)) {
      // continue only within current block
      level--;
      const int level_scale_power = (level + 1) * ScalePower;
      const uint64_t offset =
          (index & ~(~0ULL << LeafBlockPower)) >> level_scale_power;
      const uint64_t mask =
          (level == 0 && last_sample)
              ? ~(~0ULL << ((index & LevelMask[level]) >> (level * ScalePower)))
              : ~0ULL << ((index & LevelMask[level]) >> (level * ScalePower));
      uint64_t sample = (level == 0 && last_sample)
                            ? *(lbp + LevelOffset[level] + offset) | mask
                            : *(lbp + LevelOffset[level] + offset) & mask;

      // Update the low level position of the change in this block
      if (level == 0 ? sample ^ last : sample) {
        index = (index & (~0ULL << (level + 1) * ScalePower)) +
                (bsf_folded(level == 0 ? sample ^ last : sample)
                 << level * ScalePower);
        if (level == min_level)
          break;
      }
    }
  }

  return (index <= block_end);
}

bool LogicSnapshot::lbp_pre_edge(uint64_t &index, uint64_t root_index,
                                 uint64_t lbp_tog, uint8_t &lbp_tog_pos,
                                 bool pre_tog, uint8_t pre_pos,
                                 bool last_sample, int sig_index) {
  assert(lbp_tog != 0);

  // check last_sample with current index
  bool sample = get_sample_self(index, sig_index);
  if (sample ^ last_sample) {
    index++;
    return true;
  }

  // find edge between lbp
  bool edge_hit = false;
  uint64_t pre_lbp_end = (root_index << (LeafBlockPower + RootScalePower)) +
                         (pre_pos << LeafBlockPower) +
                         (1ULL << LeafBlockPower) - 1;

  do {
    uint64_t lbp_tog_index = (root_index << (LeafBlockPower + RootScalePower)) +
                             (lbp_tog_pos << LeafBlockPower) +
                             (1ULL << LeafBlockPower) - 1;
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
      lbp_tog &= (~0ULL >> (Scale - lbp_tog_pos - 1));
      lbp_tog_pos = (lbp_tog != 0) ? bsr64(lbp_tog) : 0;
    } else {
      lbp_tog = 0;
    }
  } while (lbp_tog != 0 && lbp_tog_pos >= pre_pos);

  uint64_t lbp_edge_index =
      pre_tog ? pre_lbp_end : pre_lbp_end + 1 - (1ULL << LeafBlockPower);
  if (!edge_hit && lbp_edge_index < index) {
    index = lbp_edge_index;
  }

  return edge_hit;
}

bool LogicSnapshot::block_pre_edge(uint64_t *lbp, uint64_t &index,
                                   bool last_sample, unsigned int min_level,
                                   int sig_index) {
  assert(min_level == 0);

  unsigned int level = min_level;
  bool fast_forward = true;
  const uint64_t last = last_sample ? ~0ULL : 0ULL;
  uint64_t block_start = index & ~LeafMask;

  assert(lbp);

  //----- Search Next Edge Within Current LeafBlock -----//
  if (level == 0) {
    // Search individual samples down to the beginning of
    // the previous first level mip map block
    const uint64_t offset = (index & ~(~0ULL << LeafBlockPower)) >> ScalePower;
    const uint64_t mask = last_sample
                              ? ~(~0ULL >> (Scale - (index & LevelMask[0]) - 1))
                              : ~0ULL >> (Scale - (index & LevelMask[0]) - 1);
    uint64_t sample =
        last_sample ? *(lbp + offset) | mask : *(lbp + offset) & mask;
    if (sample ^ last) {
      index =
          (index & ~LevelMask[0]) + bsr64(last_sample ? ~sample : sample) + 1;
      return true;
    } else {
      index &= ~LevelMask[0];
      if (index == 0)
        return false;
      else
        index--;

      // using get_sample_self() to avoid out of block case
      bool sample = get_sample_self(index, sig_index);
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
      const int level_scale_power = (level + 1) * ScalePower;
      const uint64_t offset =
          (index & ~(~0ULL << LeafBlockPower)) >> level_scale_power;
      const uint64_t mask =
          ~0ULL >>
          (Scale - ((index & LevelMask[level]) >> (level * ScalePower)) - 1);
      uint64_t sample = *(lbp + LevelOffset[level] + offset) & mask;

      // Check if there was a change in this block
      if (sample) {
        index = (index & (~0ULL << (level + 1) * ScalePower)) +
                (bsr64(sample) << level * ScalePower) +
                ~(~0ULL << level * ScalePower);
        break;
      } else {
        index = (index >> (level + 1) * ScalePower) << (level + 1) * ScalePower;
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
      const int level_scale_power = (level + 1) * ScalePower;
      const uint64_t offset =
          (index & ~(~0ULL << LeafBlockPower)) >> level_scale_power;
      const uint64_t mask =
          (level == 0 && last_sample)
              ? ~(~0ULL >>
                  (Scale -
                   ((index & LevelMask[level]) >> (level * ScalePower)) - 1))
              : ~0ULL >>
                    (Scale -
                     ((index & LevelMask[level]) >> (level * ScalePower)) - 1);
      uint64_t sample = (level == 0 && last_sample)
                            ? *(lbp + LevelOffset[level] + offset) | mask
                            : *(lbp + LevelOffset[level] + offset) & mask;

      // Update the low level position of the change in this block
      if (level == 0 ? sample ^ last : sample) {
        index =
            (index & (~0ULL << (level + 1) * ScalePower)) +
            (bsr64(level == 0 ? sample ^ last : sample) << level * ScalePower) +
            ~(~0ULL << level * ScalePower);
        if (level == min_level) {
          index++;
          break;
        }
      } else {
        index = (index & (~0ULL << (level + 1) * ScalePower));
      }
    }
  }

  return (index >= block_start) && (index != 0);
}

bool LogicSnapshot::pattern_search(int64_t start, int64_t end, int64_t &index,
                                   std::map<uint16_t, QString> &pattern,
                                   bool isNext) {
  std::lock_guard<std::mutex> lock(_mutex);

  start += _loop_offset;
  end += _loop_offset;
  index += _loop_offset;
  _ring_sample_count += _loop_offset;

  bool flag = pattern_search_self(start, end, index, pattern, isNext);

  index -= _loop_offset;
  _ring_sample_count -= _loop_offset;
  return flag;
}

bool LogicSnapshot::pattern_search_self(int64_t start, int64_t end,
                                        int64_t &index,
                                        std::map<uint16_t, QString> &pattern,
                                        bool isNext) {
  if (pattern.empty()) {
    return true;
  }

  char flagList[CHANNEL_MAX_COUNT];
  char lstValues[CHANNEL_MAX_COUNT];
  int chanIndexs[CHANNEL_MAX_COUNT];
  int count = 0;
  bool bEdgeFlag = false;

  const int64_t to = isNext ? end + 1 : start - 1;
  const int64_t step = isNext ? 1 : -1;

  for (auto it = pattern.begin(); it != pattern.end(); it++) {
    char flag = *(it->second.toStdString().c_str());
    int channel = it->first;

    if (flag != 'X' && has_data(channel)) {
      flagList[count] = flag;
      chanIndexs[count] = channel;
      count++;

      if (flag == 'R' || flag == 'F' || flag == 'C') {
        bEdgeFlag = true;
      }
    }
  }
  if (count == 0) {
    return true;
  }

  // find
  bool ret = false;
  char val = 0;
  int macthed = 0;

  // get first edge values
  if (bEdgeFlag) {
    for (int i = 0; i < count; i++) {
      lstValues[i] = (char)get_sample_self(index, chanIndexs[i]);
    }
    index += step;
  }

  if (index < start) {
    index = start;
  }
  if (index > end) {
    index = end;
  }

  while (index != to) {
    macthed = 0;

    for (int i = 0; i < count; i++) {
      val = (char)get_sample_self(index, chanIndexs[i]);

      if (flagList[i] == '0') {
        macthed += !val;
      } else if (flagList[i] == '1') {
        macthed += val;
      } else if (flagList[i] == 'R') {
        if (isNext)
          macthed += (lstValues[i] == 0 && val == 1);
        else
          macthed += (lstValues[i] == 1 && val == 0);
      } else if (flagList[i] == 'F') {
        if (isNext)
          macthed += (lstValues[i] == 1 && val == 0);
        else
          macthed += (lstValues[i] == 0 && val == 1);
      } else if (flagList[i] == 'C') {
        if (isNext)
          macthed += (lstValues[i] == 0 && val == 1) ||
                     (lstValues[i] == 1 && val == 0);
        else
          macthed += (lstValues[i] == 1 && val == 0) ||
                     (lstValues[i] == 0 && val == 1);
      }
      lstValues[i] = val;
    }

    // matched all
    if (macthed == count) {
      ret = true;
      if (!isNext) {
        index++; // move to prev position
      }
      break;
    }

    index += step;
  }

  return ret;
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
  assert(block_index < get_block_num());

  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size()) {
    sample = 0;
    return NULL;
  }

  int block_index0 = block_index;
  block_index += _loop_offset / LeafBlockSamples;

  uint64_t index = block_index / RootScale;
  uint8_t pos = block_index % RootScale;
  if (index >= _ch_data[order].size()) {
    sample = 0;
    return NULL;
  }
  uint8_t *lbp = (uint8_t *)_ch_data[order][index].lbp[pos];

  if (lbp == NULL) {
    sample = (_ch_data[order][index].first & 1ULL << pos) != 0;
  }

  if (lbp != NULL && _loop_offset > 0 && block_index0 == 0) {
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
      if (rn.lbp[x] != NULL) {
        push_to_free_list(rn.lbp[x]);
        rn.lbp[x] = NULL;
      }
    }

    rn.tog = 0;
    rn.first = 0;
    rn.last = 0;

    _ch_data[i].push_back(rn);
  }
}

void LogicSnapshot::decode_end() {
  std::lock_guard<std::mutex> lock(_mutex);

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
    return; // Mmap addresses are managed by MmapAllocator
  }
  _free_block_list.push_back(ptr);
}

void LogicSnapshot::free_decode_lpb(void *lbp) {
  assert(lbp);

  std::lock_guard<std::mutex> lock(_mutex);
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
  assert(count < (int)Scale);
  assert(count > 0);

  for (int i = 0; i < (int)_channel_num; i++) {
    for (int j = _lst_free_block_index; j < count; j++) {
      if (_ch_data[i][0].lbp[j] != NULL) {
        push_to_free_list(_ch_data[i][0].lbp[j]);
        _ch_data[i][0].lbp[j] = NULL;
      }

      _ch_data[i][0].tog = (_ch_data[i][0].tog >> count) << count;
      _ch_data[i][0].first = (_ch_data[i][0].first >> count) << count;
      _ch_data[i][0].last = (_ch_data[i][0].last >> count) << count;
    }
  }
  _lst_free_block_index = count;
}

int LogicSnapshot::get_block_with_sample(uint64_t index, uint64_t *out_offset) {
  assert(out_offset);

  int block = index / LeafBlockSamples;
  *out_offset = index % LeafBlockSamples;
  return block;
}

void LogicSnapshot::set_sample_range(uint64_t start, uint64_t end, bool level,
                                     int sig_index) {
  std::lock_guard<std::mutex> lock(_mutex);

  start += _loop_offset;
  end += _loop_offset;

  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size())
    return;

  uint64_t max_sample = _ring_sample_count + _loop_offset;
  if (end > max_sample)
    end = max_sample;
  if (start >= end)
    return;

  for (uint64_t pos = start; pos < end;) {
    uint64_t index0 = pos >> (LeafBlockPower + RootScalePower);
    uint64_t index1 = (pos & RootMask) >> LeafBlockPower;

    if (index0 >= _ch_data[order].size())
      break;

    uint64_t block_start = (index0 << (LeafBlockPower + RootScalePower)) +
                           (index1 << LeafBlockPower);
    uint64_t block_end = block_start + LeafBlockSamples;
    uint64_t seg_end = min(end, block_end);

    if (_ch_data[order][index0].lbp[index1] == NULL) {
      bool const_val = (_ch_data[order][index0].first & (1ULL << index1)) != 0;

      void *lbp = LeafBlockPool::instance().acquire(LeafBlockSpace);
      if (lbp == NULL) {
        _memory_failed = true;
        return;
      }

      if (const_val)
        memset(lbp, 0xFF, LeafBlockSamples / 8);
      else
        memset(lbp, 0, LeafBlockSamples / 8);

      memset((uint8_t *)lbp + LeafBlockSamples / 8, 0,
             LeafBlockSpace - LeafBlockSamples / 8);

      _ch_data[order][index0].lbp[index1] = lbp;
      _ch_data[order][index0].tog &= ~(1ULL << index1);
    }

    uint8_t *lbp = (uint8_t *)_ch_data[order][index0].lbp[index1];

    for (uint64_t i = pos; i < seg_end; i++) {
      uint64_t bit_offset = i & LeafMask;
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

void LogicSnapshot::invert_channel(int sig_index) {
  std::lock_guard<std::mutex> lock(_mutex);

  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size())
    return;

  if (_ring_sample_count == 0)
    return;



  for (uint64_t i = 0; i < _ch_data[order].size(); i++) {
    RootNode &rn = _ch_data[order][i];

    for (uint64_t j = 0; j < Scale; j++) {
      uint64_t pos_mask = 1ULL << j;

      if (rn.lbp[j] != NULL) {
        // Block has actual data — XOR all sample bytes with 0xFF
        uint8_t *lbp = (uint8_t *)rn.lbp[j];
        uint64_t sample_bytes = LeafBlockSamples / 8;

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

void LogicSnapshot::recalc_mipmap(unsigned int order, uint64_t index0,
                                  uint64_t index1) {
  void *lbp = _ch_data[order][index0].lbp[index1];

  if (lbp == NULL)
    return;

  if (index1 > 0) {
    void* prev_ptr = _ch_data[order][index0].lbp[index1 - 1];
    if (prev_ptr != NULL) {
      uint64_t *prev_lbp = (uint64_t *)prev_ptr;
      _last_sample[order] =
          (prev_lbp[LeafBlockSamples / Scale - 1] & MSB) ? ~0ULL : 0ULL;
    } else {
      bool prev_val =
          (_ch_data[order][index0].last & (1ULL << (index1 - 1))) != 0;
      _last_sample[order] = prev_val ? ~0ULL : 0ULL;
    }
  } else if (index0 > 0) {
    bool prev_val = (_ch_data[order][index0 - 1].last & MSB) != 0;
    _last_sample[order] = prev_val ? ~0ULL : 0ULL;
  } else {
    _last_sample[order] = 0;
  }

  memset((uint8_t *)lbp + LeafBlockSamples / 8, 0,
         LeafBlockSpace - LeafBlockSamples / 8);

  _ch_data[order][index0].tog &= ~(1ULL << index1);
  _ch_data[order][index0].first &= ~(1ULL << index1);
  _ch_data[order][index0].last &= ~(1ULL << index1);

  _last_calc_count[order] = 0;

  calc_mipmap(order, index0, index1, LeafBlockSamples, true);
}

LogicSnapshot *LogicSnapshot::clone_data() {
  std::lock_guard<std::mutex> lock(_mutex);

  ensure_all_blocks_hot();

  LogicSnapshot *clone = new LogicSnapshot();

  clone->_capacity = _capacity;
  clone->_channel_num = _channel_num;
  clone->_sample_count = _sample_count;
  clone->_total_sample_count = _total_sample_count;
  clone->_ring_sample_count = _ring_sample_count;
  clone->_unit_size = _unit_size;
  clone->_unit_bytes = _unit_bytes;
  clone->_unit_pitch = _unit_pitch;
  clone->_memory_failed = _memory_failed;
  clone->_last_ended = _last_ended;
  clone->_samplerate = _samplerate;
  clone->_ch_index = _ch_index;

  clone->_glitch_filtered = _glitch_filtered;

  clone->_is_loop = _is_loop;
  clone->_loop_offset = _loop_offset;
  clone->_able_free = _able_free;
  clone->_byte_fraction = _byte_fraction;
  clone->_ch_fraction = _ch_fraction;
  clone->_lst_free_block_index = _lst_free_block_index;

  memcpy(clone->_last_sample, _last_sample, sizeof(_last_sample));
  memcpy(clone->_last_calc_count, _last_calc_count, sizeof(_last_calc_count));
  memcpy(clone->_cur_ref_block_indexs, _cur_ref_block_indexs,
         sizeof(_cur_ref_block_indexs));

  clone->_ch_data.resize(_ch_data.size());
  for (size_t ch = 0; ch < _ch_data.size(); ch++) {
    clone->_ch_data[ch].resize(_ch_data[ch].size());
    for (size_t rn = 0; rn < _ch_data[ch].size(); rn++) {
      clone->_ch_data[ch][rn] = _ch_data[ch][rn];
      for (size_t lb = 0; lb < Scale; lb++) {
        if (_ch_data[ch][rn].lbp[lb] != NULL) {
          void *new_lbp = LeafBlockPool::instance().acquire(LeafBlockSpace);
          if (new_lbp == NULL) {
            clone->_memory_failed = true;
            return clone;
          }
          memcpy(new_lbp, _ch_data[ch][rn].lbp[lb], LeafBlockSpace);
          clone->_ch_data[ch][rn].lbp[lb] = new_lbp;
        }
      }
    }
  }

  clone->_dest_ptr = NULL;

  return clone;
}

void LogicSnapshot::apply_glitch_filter(
    int sig_index, uint32_t threshold,
    std::function<void(int)> progress_callback,
    GlitchFilterMode filter_mode) {
  if (threshold == 0)
    return;

  int order = get_ch_order(sig_index);
  if (order == -1 || (unsigned int)order >= _ch_data.size())
    return;

  uint64_t max_sample = _ring_sample_count;
  if (max_sample == 0)
    return;

  std::lock_guard<std::mutex> lock(_mutex);

  // 转换为绝对偏移坐标系
  _ring_sample_count += _loop_offset;

  uint64_t end_pos = max_sample + _loop_offset;
  uint64_t scan_pos = _loop_offset;

  // 状态机记录当前确认的"稳定"电平状态
  bool accepted_level = get_sample_self(scan_pos, sig_index);
  int last_progress = -1;

  pxv_info("[GlitchFilter] START sig_index=%d threshold=%u max_sample=%llu "
           "accepted_level=%d filter_mode=%d",
           sig_index, threshold, (unsigned long long)max_sample,
           accepted_level, (int)filter_mode);

  struct FillRange {
    uint64_t start;
    uint64_t end;
    bool level;
  };
  std::vector<FillRange> fills;
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
        uint64_t idx0 = pos >> (LeafBlockPower + RootScalePower);
        uint64_t idx1 = (pos & RootMask) >> LeafBlockPower;

        if (idx0 >= _ch_data[order].size())
          break;

        uint64_t block_start = (idx0 << (LeafBlockPower + RootScalePower)) +
                               (idx1 << LeafBlockPower);
        uint64_t block_end = block_start + LeafBlockSamples;
        uint64_t seg_end = min(end, block_end);

        // 如果该块尚未被实例化，则分配空间
        if (_ch_data[order][idx0].lbp[idx1] == NULL) {
          bool const_val = (_ch_data[order][idx0].first & (1ULL << idx1)) != 0;
          void *lbp = LeafBlockPool::instance().acquire(LeafBlockSpace);
          if (lbp == NULL) {
            _memory_failed = true;
            return;
          }
          if (const_val)
            memset(lbp, 0xFF, LeafBlockSamples / 8);
          else
            memset(lbp, 0, LeafBlockSamples / 8);
          memset((uint8_t *)lbp + LeafBlockSamples / 8, 0,
                 LeafBlockSpace - LeafBlockSamples / 8);
          _ch_data[order][idx0].lbp[idx1] = lbp;
        }

        uint8_t *lbp = (uint8_t *)_ch_data[order][idx0].lbp[idx1];

        // 由于马上要改写内容，此处清除该块的跳变标志位
        _ch_data[order][idx0].tog &= ~(1ULL << idx1);

        for (uint64_t i = pos; i < seg_end; i++) {
          uint64_t bit_offset = i & LeafMask;
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
    uint64_t start_blk = batch_start / LeafBlockSamples;
    uint64_t end_blk = (batch_end + LeafBlockSamples - 1) / LeafBlockSamples;

    for (uint64_t blk = start_blk; blk < end_blk; ++blk) {
      uint64_t idx0 = blk / RootScale;
      uint64_t idx1 = blk % RootScale;
      if (idx0 < _ch_data[order].size()) {
        recalc_mipmap(order, idx0, idx1);
      }
    }

    fills.clear();
  };

  while (scan_pos < end_pos) {
    bool current_scan_level = get_sample_self(scan_pos, sig_index);

    // 寻找下一个边缘（跳出当前电平）
    uint64_t edge_pos = scan_pos;
    bool found = get_nxt_edge_self(edge_pos, current_scan_level, end_pos - 1, 0,
                                   sig_index);

    if (!found) {
      pxv_info("[GlitchFilter] no more edges at scan_pos=%llu",
               (unsigned long long)scan_pos);
      break;
    }

    uint64_t pulse_start = edge_pos;
    uint64_t pulse_end = pulse_start;
    // 寻找脉冲的结束边缘（电平回归原始位置）
    bool found_end = get_nxt_edge_self(pulse_end, !current_scan_level,
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
            if (_memory_failed)
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

    int progress = (int)((scan_pos - _loop_offset) * 100 / max_sample);
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
    uint64_t vpos = _loop_offset + v;
    bool vlevel = get_sample_self(vpos, sig_index);
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
      bool vlevel = get_sample_self(vpos, sig_index);
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
  _ring_sample_count -= _loop_offset;
}

void LogicSnapshot::apply_glitch_filter_all(
    const std::vector<uint32_t> &thresholds,
    std::function<void(int)> progress_callback,
    const std::vector<GlitchFilterMode> &filter_modes) {
  for (int i = 0; i < (int)_ch_index.size(); i++) {
    if (i < (int)thresholds.size() && thresholds[i] > 0) {
      GlitchFilterMode mode = GLITCH_FILTER_BOTH;
      if (i < (int)filter_modes.size()) {
        mode = filter_modes[i];
      }
      apply_glitch_filter(_ch_index[i], thresholds[i], nullptr, mode);
    }
    if (progress_callback) {
      int progress = (i + 1) * 100 / _ch_index.size();
      progress_callback(progress);
    }
  }
  _glitch_filtered = true;
}

bool LogicSnapshot::is_glitch_filtered() { return _glitch_filtered; }

void LogicSnapshot::set_glitch_filtered(bool filtered) {
  _glitch_filtered = filtered;
}

} // namespace data
} // namespace pv
